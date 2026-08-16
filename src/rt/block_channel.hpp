#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "rt/spsc_ring.hpp"

namespace sonitude::rt
{
enum class BlockOwner : std::uint8_t
{
  // Not checked out by either side, so nobody may touch the samples. The slot is
  // either in the shared free queue or in the producer's private reserve.
  Free = 0,
  Producer = 1, // checked out by capture/DSP, which is writing it
  Ready = 2,    // in the ready queue; nobody may touch the samples
  Consumer = 3, // checked out by playback, which is reading it
};

inline const char* BlockOwnerName(const BlockOwner owner) noexcept
{
  switch (owner)
  {
  case BlockOwner::Free:
    return "free";
  case BlockOwner::Producer:
    return "producer";
  case BlockOwner::Ready:
    return "ready";
  case BlockOwner::Consumer:
    return "consumer";
  }
  return "unknown";
}

// A committed block of audio, identified by slot rather than by pointer so the
// handle stays trivially copyable and cannot outlive the pool.
struct BlockRef
{
  std::uint32_t slot = 0;
  std::uint32_t frames = 0;
};
static_assert(std::is_trivially_copyable_v<BlockRef>);

// Explicit-ownership block handoff between exactly one producer (capture/DSP)
// and exactly one consumer (playback).
//
//        free queue ──► producer ──► ready queue ──► consumer ──┐
//             ▲                                                 │
//             └─────────────────────────────────────────────────┘
//
// Invariant: every slot has exactly one owner at every instant, and the sample
// memory of a slot is only ever touched by the thread that currently owns it.
// A slot cannot be re-used until the consumer explicitly releases it, which is
// what the previous "modulo the slot count" producer could not guarantee -- it
// could rewrite a slot whose reference was still queued or being read.
//
// Both queues are strictly single-producer/single-consumer, and this is the
// property that constrains the interface:
//   * ready queue: pushed only by capture/DSP, popped only by playback.
//   * free queue:  pushed only by playback, popped only by capture/DSP.
// Nothing on the producer side may therefore push to the free queue, which is
// why a slot the producer gives up (see abandon()) is retained in a
// producer-private reserve instead of being returned through the queue.
template <typename Frame> class BlockChannel
{
  static_assert(std::is_trivially_copyable_v<Frame>,
                "audio frames are moved between threads by slot handoff and must be trivially "
                "copyable");

public:
  BlockChannel(const std::size_t slot_count, const std::size_t frames_per_slot)
      : slot_count_(Validated(slot_count, frames_per_slot)), frames_per_slot_(frames_per_slot),
        storage_(slot_count * frames_per_slot), owners_(slot_count),
        free_slots_(RingCapacityFor(slot_count)), ready_blocks_(RingCapacityFor(slot_count))
  {
    for (std::size_t i = 0; i < slot_count_; ++i)
    {
      owners_[i].store(BlockOwner::Free, std::memory_order_relaxed);
      if (!free_slots_.push(static_cast<std::uint32_t>(i)))
      {
        throw std::logic_error("BlockChannel free queue cannot hold every slot");
      }
    }
  }

  std::size_t slotCount() const noexcept
  {
    return slot_count_;
  }
  std::size_t framesPerSlot() const noexcept
  {
    return frames_per_slot_;
  }

  // ---- producer side -----------------------------------------------------

  // Takes ownership of a free slot. Returns false when playback has not
  // returned any slot yet, which is a real backpressure signal and must not be
  // hidden by adding more slots.
  bool acquire(std::uint32_t& slot) noexcept
  {
    // A slot given up by an earlier period never went back through the free
    // queue, so take it from the reserve first.
    const std::uint32_t reserved = producer_reserve_.load(std::memory_order_relaxed);
    if (reserved != kNoSlot)
    {
      producer_reserve_.store(kNoSlot, std::memory_order_relaxed);
      owners_[reserved].store(BlockOwner::Producer, std::memory_order_relaxed);
      slot = reserved;
      return true;
    }

    std::uint32_t candidate = 0;
    if (!free_slots_.pop(candidate))
    {
      pool_exhausted_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    owners_[candidate].store(BlockOwner::Producer, std::memory_order_relaxed);
    slot = candidate;
    return true;
  }

  std::span<Frame> writable(const std::uint32_t slot) noexcept
  {
    return std::span<Frame>(storage_.data() + (static_cast<std::size_t>(slot) * frames_per_slot_),
                            frames_per_slot_);
  }

  // Hands the slot to the consumer. `frames` must not exceed framesPerSlot().
  //
  // The frames_committed_ store is sequenced before the ready push, and the push
  // is a release the consumer acquires when it pops, so a consumer that can see
  // a block can always see the committed frame total that includes it. That is
  // what makes the derived occupancy in pendingFrames() unable to go negative.
  bool commit(const std::uint32_t slot, const std::size_t frames) noexcept
  {
    if (frames > frames_per_slot_)
    {
      return false;
    }
    frames_committed_.store(frames_committed_.load(std::memory_order_relaxed) +
                                static_cast<std::uint64_t>(frames),
                            std::memory_order_relaxed);
    owners_[slot].store(BlockOwner::Ready, std::memory_order_relaxed);
    const BlockRef ref{slot, static_cast<std::uint32_t>(frames)};
    if (!ready_blocks_.push(ref))
    {
      // Cannot happen: the ready queue is sized to hold every slot, and the
      // producer can hold at most one uncommitted slot. Restore ownership rather
      // than leak the slot if the invariant is ever broken.
      owners_[slot].store(BlockOwner::Producer, std::memory_order_relaxed);
      frames_committed_.store(frames_committed_.load(std::memory_order_relaxed) -
                                  static_cast<std::uint64_t>(frames),
                              std::memory_order_relaxed);
      commit_failures_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const std::size_t depth = ready_blocks_.size();
    if (depth > ready_high_water_.load(std::memory_order_relaxed))
    {
      ready_high_water_.store(depth, std::memory_order_relaxed);
    }
    return true;
  }

  // Gives up an acquired slot without publishing it (e.g. capture produced no
  // frames). Producer -> Free, but held in the producer's private reserve: the
  // producer must not push to the free queue, because playback is that queue's
  // only producer. The next acquire() hands the same slot back out.
  //
  // Returns false if a slot is already reserved, which cannot happen while the
  // producer holds at most one slot at a time.
  bool abandon(const std::uint32_t slot) noexcept
  {
    if (producer_reserve_.load(std::memory_order_relaxed) != kNoSlot)
    {
      return false;
    }
    owners_[slot].store(BlockOwner::Free, std::memory_order_relaxed);
    producer_reserve_.store(slot, std::memory_order_relaxed);
    return true;
  }

  // ---- consumer side -----------------------------------------------------

  // Takes ownership of the oldest ready block.
  bool take(BlockRef& out) noexcept
  {
    BlockRef ref{};
    if (!ready_blocks_.pop(ref))
    {
      playback_empty_waits_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    owners_[ref.slot].store(BlockOwner::Consumer, std::memory_order_relaxed);
    frames_taken_.store(frames_taken_.load(std::memory_order_relaxed) +
                            static_cast<std::uint64_t>(ref.frames),
                        std::memory_order_relaxed);
    out = ref;
    return true;
  }

  std::span<const Frame> readable(const BlockRef& ref) const noexcept
  {
    return std::span<const Frame>(
        storage_.data() + (static_cast<std::size_t>(ref.slot) * frames_per_slot_), ref.frames);
  }

  // Consumer -> Free. Until this happens the producer cannot reuse the slot.
  bool release(const BlockRef& ref) noexcept
  {
    owners_[ref.slot].store(BlockOwner::Free, std::memory_order_relaxed);
    return free_slots_.push(ref.slot);
  }

  // ---- accounting --------------------------------------------------------

  // Frames handed over but not yet taken by playback, derived from two monotone
  // counters that each have exactly one writer. Call it from the consumer: it is
  // the only writer of frames_taken_, and every block it has taken was committed
  // before it could be popped, so the subtraction cannot underflow.
  //
  // This is the only software-queue occupancy figure in the system, so there is
  // no second counter that can disagree with the queues about who owns what. It
  // is not instantaneously exact, and deliberately so: commit() bumps
  // frames_committed_ before pushing the block, so a consumer reading between
  // those two operations counts one block that it cannot yet pop. The error is
  // bounded by one period, always in the direction of over-reporting occupancy,
  // and it feeds the ASRC ratio estimate rather than any ownership decision.
  std::uint64_t pendingFrames() const noexcept
  {
    return frames_committed_.load(std::memory_order_acquire) -
           frames_taken_.load(std::memory_order_relaxed);
  }

  std::size_t readyCount() const noexcept
  {
    return ready_blocks_.size();
  }
  std::size_t freeCount() const noexcept
  {
    const bool reserved = producer_reserve_.load(std::memory_order_relaxed) != kNoSlot;
    return free_slots_.size() + (reserved ? 1U : 0U);
  }
  BlockOwner ownerOf(const std::size_t slot) const noexcept
  {
    return owners_[slot].load(std::memory_order_relaxed);
  }

  std::uint64_t framesCommitted() const noexcept
  {
    return frames_committed_.load(std::memory_order_relaxed);
  }
  std::uint64_t framesTaken() const noexcept
  {
    return frames_taken_.load(std::memory_order_relaxed);
  }
  std::uint64_t poolExhaustedCount() const noexcept
  {
    return pool_exhausted_.load(std::memory_order_relaxed);
  }
  std::uint64_t playbackEmptyWaitCount() const noexcept
  {
    return playback_empty_waits_.load(std::memory_order_relaxed);
  }
  std::uint64_t commitFailureCount() const noexcept
  {
    return commit_failures_.load(std::memory_order_relaxed);
  }
  std::size_t readyHighWater() const noexcept
  {
    return ready_high_water_.load(std::memory_order_relaxed);
  }

private:
  static constexpr std::uint32_t kNoSlot = ~static_cast<std::uint32_t>(0);

  static std::size_t Validated(const std::size_t slot_count, const std::size_t frames_per_slot)
  {
    if (slot_count < 2U || frames_per_slot == 0U)
    {
      throw std::invalid_argument("BlockChannel needs at least two slots and a non-zero period");
    }
    return slot_count;
  }

  // Both queues must be able to hold every slot at once, and SpscRing reserves
  // one element, so round slot_count + 1 up to a power of two.
  static std::size_t RingCapacityFor(const std::size_t slot_count)
  {
    std::size_t capacity = 2;
    while (capacity < slot_count + 1U)
    {
      capacity <<= 1U;
    }
    return capacity;
  }

  std::size_t slot_count_;
  std::size_t frames_per_slot_;
  std::vector<Frame> storage_;
  std::vector<std::atomic<BlockOwner>> owners_;
  SpscRing<std::uint32_t> free_slots_;
  SpscRing<BlockRef> ready_blocks_;

  // Written by the producer only. Atomic solely so telemetry can read it for
  // freeCount(); the producer is its only writer and reads it relaxed.
  std::atomic<std::uint32_t> producer_reserve_{kNoSlot};
  // Written by the producer only.
  std::atomic<std::uint64_t> frames_committed_{0};
  std::atomic<std::uint64_t> pool_exhausted_{0};
  std::atomic<std::uint64_t> commit_failures_{0};
  std::atomic<std::size_t> ready_high_water_{0};
  // Written by the consumer only.
  std::atomic<std::uint64_t> frames_taken_{0};
  // Count of consumer checks that found no ready software block before waiting.
  std::atomic<std::uint64_t> playback_empty_waits_{0};

  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "block accounting is updated by realtime threads and must be lock-free");
  static_assert(std::atomic<BlockOwner>::is_always_lock_free,
                "slot ownership state is updated by realtime threads and must be lock-free");
};
} // namespace sonitude::rt
