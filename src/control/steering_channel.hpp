#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "control/rt_steering_snapshot.hpp"
#include "rt/spsc_ring.hpp"

namespace sonitude::control
{
// Bounded control -> audio channel. One producer (the control thread), one
// consumer (the audio thread).
//
// This replaces the previous seqlock-style snapshot buffer. The reason is a
// memory-model one, not a stylistic one: a seqlock reads ordinary non-atomic
// object memory while the writer may be modifying it, and a sequence re-check
// afterwards cannot retroactively make that access well-defined -- it is a data
// race, which is undefined behaviour. Here a slot is only ever touched by one
// thread at a time, and the ring's release/acquire pair on the index supplies
// the happens-before edge for the payload.
//
// Latest-wins semantics: the audio thread drains everything queued at a period
// boundary and keeps the newest message. Losing superseded intermediate steering
// updates is harmless because each is a complete description of the desired
// state, not a delta.
class SteeringChannel
{
 public:
  static constexpr std::size_t kDefaultCapacity = 16;

  explicit SteeringChannel(std::size_t capacity_pow2 = kDefaultCapacity) : ring_(capacity_pow2) {}

  // Control thread. Returns false when the queue is full, which is recorded and
  // is self-healing: the control loop republishes complete state every tick, so
  // a refused publication delays an update by one tick and loses nothing
  // permanently.
  bool publish(const RtSteeringSnapshot& snapshot) noexcept
  {
    if (!ring_.push(snapshot))
    {
      publish_drops_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    published_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Audio thread. Drains all queued messages and writes the newest into `out`.
  // Returns false (leaving `out` untouched) when nothing new was queued, so the
  // caller keeps using its last valid message.
  bool drainLatest(RtSteeringSnapshot& out) noexcept
  {
    RtSteeringSnapshot message{};
    std::uint32_t drained = 0;
    while (ring_.pop(message))
    {
      ++drained;
    }
    if (drained == 0U)
    {
      return false;
    }
    if (drained > 1U)
    {
      superseded_.fetch_add(drained - 1U, std::memory_order_relaxed);
    }
    out = message;
    return true;
  }

  // Test/diagnostic accessor: takes exactly one message.
  bool tryPop(RtSteeringSnapshot& out) noexcept { return ring_.pop(out); }

  std::size_t capacityUsable() const noexcept { return ring_.capacityUsable(); }

  // Telemetry only; relaxed is sufficient because nothing is ordered against
  // these counters.
  std::uint64_t published() const noexcept { return published_.load(std::memory_order_relaxed); }
  std::uint64_t publishDrops() const noexcept
  {
    return publish_drops_.load(std::memory_order_relaxed);
  }
  std::uint64_t superseded() const noexcept { return superseded_.load(std::memory_order_relaxed); }

 private:
  rt::SpscRing<RtSteeringSnapshot> ring_;
  std::atomic<std::uint64_t> published_{0};
  std::atomic<std::uint64_t> publish_drops_{0};
  std::atomic<std::uint64_t> superseded_{0};
};
}  // namespace sonitude::control
