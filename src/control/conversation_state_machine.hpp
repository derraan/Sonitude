#pragma once

#include <array>
#include <cstdint>

#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "control/steering_snapshot.hpp"
#include "control/zones.hpp"
#include "spatial/angles.hpp"

namespace sonitude::control
{
enum class ConversationState : std::uint8_t
{
  Ambient = 0,
  Candidate = 1,
  Focused = 2,
  Held = 3,
  Releasing = 4,
  kCount = 5
};

struct ConversationInput
{
  bool has_track = false;
  audio::BeamformerSteering track{};
  float confidence = 0.0F;
  float speech_probability = 0.0F;
};

class ConversationStateMachine
{
 public:
  ConversationStateMachine(app::StateMachineConfig config, float ambient_floor_linear, ZoneMap zones);

  SteeringSnapshot update(const ConversationInput& input, std::uint64_t now_ns);
  ConversationState state() const { return state_; }
  std::uint64_t transitionCount(ConversationState from, ConversationState to) const;

 private:
  void transitionTo(ConversationState next);
  bool directionStable(float azimuth_deg) const;
  bool isFocusEligible(const std::optional<ResolvedZone>& zone) const;
  bool isAmbientZone(const std::optional<ResolvedZone>& zone) const;

  app::StateMachineConfig config_{};
  float ambient_floor_linear_ = 0.25F;
  ZoneMap zones_;

  ConversationState state_ = ConversationState::Ambient;
  std::array<std::uint64_t, static_cast<std::size_t>(ConversationState::kCount) *
                                static_cast<std::size_t>(ConversationState::kCount)>
      transitions_{};

  std::uint64_t state_entry_ns_ = 0;
  audio::BeamformerSteering focus_target_{};
  std::uint64_t generation_ = 0;
};
}  // namespace sonitude::control
