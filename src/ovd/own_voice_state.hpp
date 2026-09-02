#pragma once

#include <cstdint>
#include <type_traits>

namespace sonitude::ovd
{
enum class OwnVoiceHealth : std::uint8_t
{
  Disabled = 0,
  Healthy = 1,
  ModelUnavailable = 2,
  InvalidObservation = 3,
  Stale = 4
};

// Complete slow/control -> RT message. It is an orthogonal control input and
// deliberately does not multiply ConversationState variants.
struct OwnVoiceState
{
  // Distance-derived similarity score in [0, 1]; not a posterior probability.
  float probability = 0.0F;
  std::uint64_t generation = 0;
  std::uint64_t observed_ns = 0;
  bool active = false;
  OwnVoiceHealth health = OwnVoiceHealth::Disabled;
};

inline bool IsFreshAndHealthy(const OwnVoiceState& state, const std::uint64_t now_ns,
                              const std::uint64_t stale_timeout_ns) noexcept
{
  return state.health == OwnVoiceHealth::Healthy && now_ns >= state.observed_ns &&
         (now_ns - state.observed_ns) <= stale_timeout_ns;
}

static_assert(std::is_trivially_copyable_v<OwnVoiceState>,
              "OwnVoiceState crosses into realtime and must be trivially copyable");
static_assert(std::is_trivially_destructible_v<OwnVoiceState>,
              "OwnVoiceState must not run a destructor on the realtime thread");
static_assert(std::is_standard_layout_v<OwnVoiceState>,
              "OwnVoiceState must remain a flat scalar aggregate");
} // namespace sonitude::ovd
