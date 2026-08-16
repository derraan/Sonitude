#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ovd/own_voice_features.hpp"
#include "ovd/own_voice_state.hpp"
#include "rt/spsc_ring.hpp"

namespace sonitude::ovd
{
// Audio producer -> OVD worker consumer. Full queue policy is drop-newest:
// the audio deadline never waits for OVD.
class OwnVoiceObservationChannel
{
public:
  explicit OwnVoiceObservationChannel(const std::size_t capacity_pow2 = 16U)
      : ring_(capacity_pow2)
  {
  }

  bool publish(const OwnVoiceObservation& observation) noexcept
  {
    if (!ring_.push(observation))
    {
      drops_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    published_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  bool tryPop(OwnVoiceObservation& observation) noexcept
  {
    return ring_.pop(observation);
  }

  std::uint64_t drops() const noexcept
  {
    return drops_.load(std::memory_order_relaxed);
  }

private:
  rt::SpscRing<OwnVoiceObservation> ring_;
  std::atomic<std::uint64_t> published_{0};
  std::atomic<std::uint64_t> drops_{0};
};

// OVD/control producer -> audio consumer. Each state is complete; audio drains
// superseded states at a period boundary and retains only the newest.
class OwnVoiceStateChannel
{
public:
  explicit OwnVoiceStateChannel(const std::size_t capacity_pow2 = 16U) : ring_(capacity_pow2) {}

  bool publish(const OwnVoiceState& state) noexcept
  {
    if (!ring_.push(state))
    {
      drops_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    return true;
  }

  bool drainLatest(OwnVoiceState& state) noexcept
  {
    OwnVoiceState candidate{};
    std::uint64_t drained = 0;
    while (ring_.pop(candidate))
    {
      ++drained;
    }
    if (drained == 0U)
    {
      return false;
    }
    state = candidate;
    if (drained > 1U)
    {
      superseded_.fetch_add(drained - 1U, std::memory_order_relaxed);
    }
    return true;
  }

  std::uint64_t drops() const noexcept
  {
    return drops_.load(std::memory_order_relaxed);
  }

private:
  rt::SpscRing<OwnVoiceState> ring_;
  std::atomic<std::uint64_t> drops_{0};
  std::atomic<std::uint64_t> superseded_{0};
};
} // namespace sonitude::ovd
