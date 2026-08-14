#include <atomic>
#include <barrier>
#include <cstdint>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "control/rt_steering_snapshot.hpp"
#include "control/steering_channel.hpp"
#include "rt/block_channel.hpp"
#include "rt/spsc_ring.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

struct TestFrame
{
  std::uint64_t tag = 0;
};

using TestChannel = sonitude::rt::BlockChannel<TestFrame>;

// ---- compile-time realtime message contract --------------------------------

static_assert(std::is_trivially_copyable_v<sonitude::control::RtSteeringSnapshot>,
              "the realtime steering message must be trivially copyable");
static_assert(std::is_trivially_destructible_v<sonitude::control::RtSteeringSnapshot>,
              "the realtime steering message must not run a destructor");
static_assert(std::is_trivially_copyable_v<sonitude::rt::BlockRef>,
              "a block handle must be trivially copyable");
static_assert(sizeof(sonitude::control::RtSteeringSnapshot) <= 64,
              "the realtime steering message should stay small enough to copy cheaply");

void TestRtMessageHasNoOwningMembers()
{
  // A trivially copyable type cannot own heap memory through a standard owning
  // member, so this is mostly a guard against the field being reintroduced. The
  // static_asserts above are the real enforcement; this documents intent.
  sonitude::control::RtSteeringSnapshot snapshot{};
  Require(snapshot.failsafe, "a default realtime snapshot must be failsafe");
  Require(snapshot.zone_id == sonitude::control::kNoZoneId,
          "a default realtime snapshot must have no zone");
}

// ---- ring behaviour --------------------------------------------------------

void TestRingRejectsBadCapacity()
{
  bool threw = false;
  try
  {
    sonitude::rt::SpscRing<std::uint64_t> ring(12);
  }
  catch (const std::invalid_argument&)
  {
    threw = true;
  }
  Require(threw, "a non-power-of-two capacity must be rejected");

  threw = false;
  try
  {
    sonitude::rt::SpscRing<std::uint64_t> ring(1);
  }
  catch (const std::invalid_argument&)
  {
    threw = true;
  }
  Require(threw, "a capacity below two must be rejected");
}

void TestRingFullAndEmptyBoundaries()
{
  sonitude::rt::SpscRing<std::uint64_t> ring(8);
  Require(ring.capacityUsable() == 7U, "one slot is reserved to distinguish full from empty");
  std::uint64_t out = 0;
  Require(!ring.pop(out), "an empty ring must refuse to pop");
  for (std::uint64_t i = 0; i < ring.capacityUsable(); ++i)
  {
    Require(ring.push(i), "the ring must accept up to its usable capacity");
  }
  Require(!ring.push(999), "a full ring must refuse to push");
  Require(ring.size() == ring.capacityUsable(), "a full ring must report its usable capacity");
  for (std::uint64_t i = 0; i < ring.capacityUsable(); ++i)
  {
    Require(ring.pop(out) && out == i, "the ring must preserve order");
  }
  Require(!ring.pop(out), "a drained ring must report empty");
  Require(ring.empty(), "a drained ring must be empty");
  // A wrapped ring must behave identically.
  Require(ring.push(42), "the ring must be reusable after wrapping");
  Require(ring.pop(out) && out == 42, "the wrapped ring must return the value");
}

// ---- block ownership -------------------------------------------------------

void TestBlockOwnershipTransitions()
{
  TestChannel channel(4, 8);
  Require(channel.slotCount() == 4U, "slot count must be as constructed");
  Require(channel.freeCount() == 4U, "every slot must start free");
  for (std::size_t i = 0; i < channel.slotCount(); ++i)
  {
    Require(channel.ownerOf(i) == sonitude::rt::BlockOwner::Free, "slots must start free");
  }

  std::uint32_t slot = 0;
  Require(channel.acquire(slot), "acquire must succeed while slots are free");
  Require(channel.ownerOf(slot) == sonitude::rt::BlockOwner::Producer,
          "an acquired slot must be owned by the producer");
  Require(channel.freeCount() == 3U, "acquire must remove the slot from the free queue");

  channel.writable(slot)[0].tag = 7;
  Require(channel.commit(slot, 8), "commit must succeed for a full block");
  Require(channel.ownerOf(slot) == sonitude::rt::BlockOwner::Ready,
          "a committed slot must be owned by the ready queue");
  Require(channel.readyCount() == 1U, "commit must enqueue the block");

  sonitude::rt::BlockRef ref{};
  Require(channel.take(ref), "take must return the committed block");
  Require(ref.slot == slot && ref.frames == 8U, "take must return the committed identity");
  Require(channel.ownerOf(slot) == sonitude::rt::BlockOwner::Consumer,
          "a taken slot must be owned by the consumer");
  Require(channel.readable(ref)[0].tag == 7U, "the consumer must see the produced payload");
  Require(channel.freeCount() == 3U, "a slot in flight must not be free");

  Require(channel.release(ref), "release must succeed");
  Require(channel.ownerOf(slot) == sonitude::rt::BlockOwner::Free,
          "a released slot must return to free");
  Require(channel.freeCount() == 4U, "release must return the slot to the free queue");
}

void TestCommitRejectsOversizedBlock()
{
  TestChannel channel(4, 8);
  std::uint32_t slot = 0;
  Require(channel.acquire(slot), "acquire must succeed");
  Require(!channel.commit(slot, 9), "a commit larger than the slot must be refused");
  Require(channel.ownerOf(slot) == sonitude::rt::BlockOwner::Producer,
          "a refused commit must leave the slot with the producer");
  Require(channel.abandon(slot), "the producer must be able to hand the slot back");
  Require(channel.ownerOf(slot) == sonitude::rt::BlockOwner::Free,
          "an abandoned slot must return to free");
  Require(channel.freeCount() == 4U, "an abandoned slot must be counted as free again");

  // The slot must come back from the producer's own reserve. If abandon() had
  // pushed it into the free queue, the producer would be a second writer of a
  // queue the consumer also pushes to, which breaks the SPSC contract.
  std::uint32_t reacquired = 0;
  Require(channel.acquire(reacquired), "the reserved slot must be available again");
  Require(reacquired == slot, "acquire must hand back the reserved slot itself");
  Require(channel.freeCount() == 3U, "reacquiring the reserve must reduce the free count");
}

// The producer must never touch the free queue's push side. Exercised by
// abandoning on the producer thread while the consumer releases concurrently:
// under ThreadSanitizer, a producer-side push would be reported as a race on the
// queue's head index.
void TestAbandonDoesNotRaceConsumerRelease()
{
  constexpr std::size_t kIterations = 20000;
  TestChannel channel(8, 4);
  std::atomic<bool> go{false};
  std::atomic<std::size_t> released{0};

  std::thread consumer(
      [&]
      {
        while (!go.load(std::memory_order_acquire))
        {
        }
        std::size_t seen = 0;
        while (seen < kIterations)
        {
          sonitude::rt::BlockRef ref{};
          if (channel.take(ref))
          {
            Require(channel.release(ref), "release must succeed");
            ++seen;
          }
        }
        released.store(seen, std::memory_order_release);
      });

  go.store(true, std::memory_order_release);
  std::size_t committed = 0;
  while (committed < kIterations)
  {
    std::uint32_t slot = 0;
    if (!channel.acquire(slot))
    {
      continue;
    }
    // Alternate between publishing and giving the slot straight back, so the
    // reserve path runs against a live consumer.
    if ((committed & 1U) == 0U)
    {
      Require(channel.abandon(slot), "abandon must succeed when no slot is reserved");
      std::uint32_t again = 0;
      Require(channel.acquire(again) && again == slot, "the reserve must be returned");
      slot = again;
    }
    Require(channel.commit(slot, 4), "commit must succeed");
    ++committed;
  }
  consumer.join();
  Require(released.load(std::memory_order_acquire) == kIterations,
          "the consumer must observe every committed block");
  Require(channel.framesCommitted() == channel.framesTaken(),
          "committed and taken frame totals must agree once the pipeline drains");
}

void TestProducerBlocksWhenConsumerNeverReleases()
{
  // Producer outrunning playback: the pool must run dry rather than overwrite a
  // slot that the consumer still owns.
  TestChannel channel(4, 4);
  std::vector<sonitude::rt::BlockRef> held;
  for (std::size_t i = 0; i < channel.slotCount(); ++i)
  {
    std::uint32_t slot = 0;
    Require(channel.acquire(slot), "the producer must be able to fill the pool");
    Require(channel.commit(slot, 4), "commit must succeed");
    sonitude::rt::BlockRef ref{};
    Require(channel.take(ref), "the consumer takes the block but does not release it");
    held.push_back(ref);
  }

  std::uint32_t overflow_slot = 0;
  Require(!channel.acquire(overflow_slot),
          "with every slot held by the consumer, acquire must fail");
  Require(channel.poolExhaustedCount() == 1U, "pool exhaustion must be counted");

  // Every slot is accounted for, and no two owners claim the same slot.
  for (const sonitude::rt::BlockRef& ref : held)
  {
    Require(channel.ownerOf(ref.slot) == sonitude::rt::BlockOwner::Consumer,
            "held slots must remain owned by the consumer");
  }
  for (const sonitude::rt::BlockRef& ref : held)
  {
    Require(channel.release(ref), "release must succeed");
  }
  Require(channel.acquire(overflow_slot), "after release the producer can proceed");
}

void TestConsumerStarvesWhenProducerIsSlow()
{
  // Playback outrunning the producer: take() must report starvation rather than
  // hand back a stale or half-written block.
  TestChannel channel(4, 4);
  sonitude::rt::BlockRef ref{};
  Require(!channel.take(ref), "an empty ready queue must not yield a block");
  Require(channel.starvationCount() == 1U, "starvation must be counted");

  std::uint32_t slot = 0;
  Require(channel.acquire(slot), "acquire must succeed");
  // Acquired but not committed: still invisible to the consumer.
  Require(!channel.take(ref), "an uncommitted block must not be visible to the consumer");
  Require(channel.commit(slot, 2), "commit must succeed");
  Require(channel.take(ref), "a committed block must become visible");
  Require(ref.frames == 2U, "partial blocks must carry their real frame count");
  Require(channel.release(ref), "release must succeed");
}

void TestOccupancyNeverUnderflows()
{
  TestChannel channel(4, 4);
  Require(channel.pendingFrames() == 0U, "an idle channel has no pending frames");

  std::uint32_t slot = 0;
  Require(channel.acquire(slot), "acquire must succeed");
  Require(channel.commit(slot, 3), "commit must succeed");
  Require(channel.pendingFrames() == 3U, "committed frames must be pending");

  sonitude::rt::BlockRef ref{};
  Require(channel.take(ref), "take must succeed");
  // The consumer accounts for the block as soon as it takes ownership, so the
  // block it is about to write is no longer "pending".
  Require(channel.pendingFrames() == 0U, "a taken block must leave the pending count");
  Require(channel.release(ref), "release must succeed");
  Require(channel.pendingFrames() == 0U, "release must not change the pending count");
  Require(channel.framesCommitted() == 3U && channel.framesTaken() == 3U,
          "committed and taken totals must agree once drained");
}

// The scenario that made the old design unsafe: the producer wraps around the
// slot count many times while the consumer is concurrently reading. Every block
// the consumer sees must be internally consistent, which can only hold if the
// producer never touched a slot the consumer owned.
void TestConcurrentOwnershipUnderWrapAround()
{
  constexpr std::size_t kSlots = 4;
  constexpr std::size_t kFrames = 16;
  constexpr std::uint64_t kBlocks = 20000;
  TestChannel channel(kSlots, kFrames);

  std::atomic<bool> corruption{false};
  std::atomic<std::uint64_t> consumed{0};
  std::latch start(2);

  std::thread producer(
      [&]
      {
        start.arrive_and_wait();
        for (std::uint64_t i = 1; i <= kBlocks; ++i)
        {
          std::uint32_t slot = 0;
          while (!channel.acquire(slot))
          {
            std::this_thread::yield();
          }
          const std::span<TestFrame> frames = channel.writable(slot);
          // Stamp every frame with the same tag; a torn block shows up as a frame
          // that disagrees with its neighbours.
          for (std::size_t f = 0; f < kFrames; ++f)
          {
            frames[f].tag = i;
          }
          while (!channel.commit(slot, kFrames))
          {
            std::this_thread::yield();
          }
        }
      });

  std::thread consumer(
      [&]
      {
        start.arrive_and_wait();
        std::uint64_t previous_tag = 0;
        while (consumed.load(std::memory_order_relaxed) < kBlocks)
        {
          sonitude::rt::BlockRef ref{};
          if (!channel.take(ref))
          {
            std::this_thread::yield();
            continue;
          }
          const std::span<const TestFrame> frames = channel.readable(ref);
          const std::uint64_t tag = frames[0].tag;
          for (const TestFrame& frame : frames)
          {
            if (frame.tag != tag)
            {
              corruption.store(true, std::memory_order_relaxed);
            }
          }
          if (tag <= previous_tag)
          {
            corruption.store(true, std::memory_order_relaxed);
          }
          previous_tag = tag;
          // Occupancy is derived, and must never wrap even under contention.
          if (channel.pendingFrames() > kSlots * kFrames)
          {
            corruption.store(true, std::memory_order_relaxed);
          }
          if (!channel.release(ref))
          {
            corruption.store(true, std::memory_order_relaxed);
          }
          consumed.fetch_add(1, std::memory_order_relaxed);
        }
      });

  producer.join();
  consumer.join();
  Require(!corruption.load(std::memory_order_relaxed),
          "no block may be torn, reordered, or double-owned across slot reuse");
  Require(consumed.load(std::memory_order_relaxed) == kBlocks,
          "every committed block must be delivered exactly once");
  Require(channel.framesCommitted() == channel.framesTaken(),
          "committed and taken frame totals must agree at the end");
  Require(channel.pendingFrames() == 0U, "no frames may remain pending");
  for (std::size_t i = 0; i < channel.slotCount(); ++i)
  {
    Require(channel.ownerOf(i) == sonitude::rt::BlockOwner::Free,
            "every slot must be free once the pipeline drains");
  }
}

// Deliberately slow consumer: the producer must degrade by dropping blocks it
// cannot place, never by reusing a slot that is still owned elsewhere.
void TestStressWithDelayedConsumer()
{
  constexpr std::size_t kSlots = 4;
  constexpr std::size_t kFrames = 8;
  constexpr std::uint64_t kAttempts = 5000;
  TestChannel channel(kSlots, kFrames);

  std::atomic<bool> corruption{false};
  std::atomic<bool> producer_done{false};
  std::atomic<std::uint64_t> committed{0};
  std::atomic<std::uint64_t> dropped{0};
  std::latch start(2);

  std::thread producer(
      [&]
      {
        start.arrive_and_wait();
        for (std::uint64_t i = 1; i <= kAttempts; ++i)
        {
          std::uint32_t slot = 0;
          if (!channel.acquire(slot))
          {
            // This is the correct degradation: drop the block.
            dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          const std::span<TestFrame> frames = channel.writable(slot);
          for (std::size_t f = 0; f < kFrames; ++f)
          {
            frames[f].tag = i;
          }
          if (channel.commit(slot, kFrames))
          {
            committed.fetch_add(1, std::memory_order_relaxed);
          }
          else
          {
            corruption.store(true, std::memory_order_relaxed);
          }
        }
        producer_done.store(true, std::memory_order_release);
      });

  std::thread consumer(
      [&]
      {
        start.arrive_and_wait();
        std::uint64_t taken = 0;
        for (;;)
        {
          sonitude::rt::BlockRef ref{};
          if (!channel.take(ref))
          {
            if (producer_done.load(std::memory_order_acquire) && channel.readyCount() == 0U)
            {
              break;
            }
            std::this_thread::yield();
            continue;
          }
          const std::span<const TestFrame> frames = channel.readable(ref);
          const std::uint64_t tag = frames[0].tag;
          for (const TestFrame& frame : frames)
          {
            if (frame.tag != tag)
            {
              corruption.store(true, std::memory_order_relaxed);
            }
          }
          if (!channel.release(ref))
          {
            corruption.store(true, std::memory_order_relaxed);
          }
          ++taken;
          // Hold the block long enough that the producer really does run ahead.
          if ((taken % 64U) == 0U)
          {
            std::this_thread::yield();
          }
        }
      });

  producer.join();
  consumer.join();
  Require(!corruption.load(std::memory_order_relaxed),
          "a delayed consumer must not cause block corruption");
  Require(committed.load(std::memory_order_relaxed) + dropped.load(std::memory_order_relaxed) ==
              kAttempts,
          "every block must be either committed or explicitly dropped");
  Require(channel.framesCommitted() == channel.framesTaken(),
          "frame accounting must balance after a stress run");
  for (std::size_t i = 0; i < channel.slotCount(); ++i)
  {
    Require(channel.ownerOf(i) == sonitude::rt::BlockOwner::Free,
            "no slot may be left checked out after a stress run");
  }
}

// ---- control channel -------------------------------------------------------

void TestSteeringChannelLatestWins()
{
  sonitude::control::SteeringChannel channel(8);
  sonitude::control::RtSteeringSnapshot out{};
  Require(!channel.drainLatest(out), "an empty channel must report no update");

  for (std::uint64_t generation = 1; generation <= 3; ++generation)
  {
    sonitude::control::RtSteeringSnapshot message{};
    message.generation = generation;
    message.target.azimuth_deg = static_cast<float>(generation);
    Require(channel.publish(message), "publish must succeed while the channel has room");
  }

  Require(channel.drainLatest(out), "drain must report an update");
  Require(out.generation == 3U, "drain must yield the newest message");
  Require(channel.superseded() == 2U, "superseded intermediate messages must be counted");

  // With nothing new queued, the caller keeps its last value untouched.
  sonitude::control::RtSteeringSnapshot retained = out;
  Require(!channel.drainLatest(retained), "a drained channel must report no update");
  Require(retained.generation == 3U, "the previous message must be retained unchanged");
}

void TestSteeringChannelFullIsCountedNotFatal()
{
  sonitude::control::SteeringChannel channel(4);
  Require(channel.capacityUsable() == 3U, "usable capacity is one below the ring size");
  for (std::size_t i = 0; i < channel.capacityUsable(); ++i)
  {
    Require(channel.publish({}), "publish must succeed until the channel is full");
  }
  Require(!channel.publish({}), "a full channel must refuse the publication");
  Require(channel.publishDrops() == 1U, "a refused publication must be counted");

  sonitude::control::RtSteeringSnapshot out{};
  Require(channel.drainLatest(out), "draining must free the channel again");
  Require(channel.publish({}), "publication must recover after a drain");
}

// Control publishing while audio consumes: the audio side must only ever see
// whole messages, and must never go backwards in generation.
void TestConcurrentControlPublication()
{
  constexpr std::uint64_t kUpdates = 50000;
  sonitude::control::SteeringChannel channel(16);
  std::atomic<bool> corruption{false};
  std::atomic<bool> producer_done{false};
  std::latch start(2);

  std::thread control(
      [&]
      {
        start.arrive_and_wait();
        for (std::uint64_t generation = 1; generation <= kUpdates; ++generation)
        {
          sonitude::control::RtSteeringSnapshot message{};
          message.generation = generation;
          // Two fields derived from the same value: a torn message would disagree.
          message.target.azimuth_deg = static_cast<float>(generation % 360U);
          message.confidence = static_cast<float>(generation % 360U);
          message.failsafe = (generation % 2U) == 0U;
          message.has_distractor = (generation % 2U) == 0U;
          while (!channel.publish(message))
          {
            std::this_thread::yield();
          }
        }
        producer_done.store(true, std::memory_order_release);
      });

  std::thread audio(
      [&]
      {
        start.arrive_and_wait();
        sonitude::control::RtSteeringSnapshot current{};
        std::uint64_t last_generation = 0;
        for (;;)
        {
          const bool updated = channel.drainLatest(current);
          if (updated)
          {
            if (current.target.azimuth_deg != current.confidence ||
                current.failsafe != current.has_distractor)
            {
              corruption.store(true, std::memory_order_relaxed);
            }
            if (current.generation <= last_generation)
            {
              corruption.store(true, std::memory_order_relaxed);
            }
            last_generation = current.generation;
          }
          if (producer_done.load(std::memory_order_acquire) && !updated)
          {
            // One last drain to pick up anything published just before the flag.
            if (!channel.drainLatest(current))
            {
              break;
            }
            last_generation = current.generation;
          }
        }
        if (last_generation != kUpdates)
        {
          corruption.store(true, std::memory_order_relaxed);
        }
      });

  control.join();
  audio.join();
  Require(!corruption.load(std::memory_order_relaxed),
          "control publication must never be observed torn or out of order");
}
} // namespace

void RunThreadSafetyTests()
{
  TestRtMessageHasNoOwningMembers();
  TestRingRejectsBadCapacity();
  TestRingFullAndEmptyBoundaries();
  TestBlockOwnershipTransitions();
  TestCommitRejectsOversizedBlock();
  TestAbandonDoesNotRaceConsumerRelease();
  TestProducerBlocksWhenConsumerNeverReleases();
  TestConsumerStarvesWhenProducerIsSlow();
  TestOccupancyNeverUnderflows();
  TestConcurrentOwnershipUnderWrapAround();
  TestStressWithDelayedConsumer();
  TestSteeringChannelLatestWins();
  TestSteeringChannelFullIsCountedNotFatal();
  TestConcurrentControlPublication();
}
