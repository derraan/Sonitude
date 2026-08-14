#include "control/conversation_state_machine.hpp"

#include <cmath>
#include <optional>
#include <utility>

namespace sonitude::control
{
namespace
{
std::size_t Idx(const ConversationState from, const ConversationState to)
{
  const std::size_t n = static_cast<std::size_t>(ConversationState::kCount);
  return static_cast<std::size_t>(from) * n + static_cast<std::size_t>(to);
}
}  // namespace

ConversationStateMachine::ConversationStateMachine(const app::StateMachineConfig config,
                                                   const float ambient_floor_linear,
                                                   ZoneMap zones)
    : config_(config), ambient_floor_linear_(ambient_floor_linear), zones_(std::move(zones))
{
}

void ConversationStateMachine::transitionTo(const ConversationState next)
{
  transitions_[Idx(state_, next)] += 1;
  state_ = next;
}

bool ConversationStateMachine::directionStable(const float azimuth_deg) const
{
  const double diff =
      std::fabs(spatial::SignedAngularDistanceDeg(focus_target_.azimuth_deg, azimuth_deg));
  return diff <= static_cast<double>(config_.zone_direction_stability_deg);
}

bool ConversationStateMachine::isFocusEligible(const std::optional<ResolvedZone>& zone)
{
  return zone.has_value() && zone->policy != app::ZonePolicy::Ambient;
}

bool ConversationStateMachine::isAmbientZone(const std::optional<ResolvedZone>& zone)
{
  return zone.has_value() && zone->policy == app::ZonePolicy::Ambient;
}

SteeringSnapshot ConversationStateMachine::update(const ConversationInput& input,
                                                  const std::uint64_t now_ns)
{
  if (state_entry_ns_ == 0)
  {
    state_entry_ns_ = now_ns;
  }

  const bool speech = input.speech_probability >= 0.6F;
  const std::optional<ResolvedZone> zone =
      input.has_track ? zones_.resolve(input.track.azimuth_deg) : std::nullopt;
  switch (state_)
  {
    case ConversationState::Ambient:
      if (input.has_track && speech && isFocusEligible(zone))
      {
        focus_target_ = input.track;
        transitionTo(ConversationState::Candidate);
        state_entry_ns_ = now_ns;
      }
      break;
    case ConversationState::Candidate:
      if (!(input.has_track && speech && isFocusEligible(zone)))
      {
        transitionTo(ConversationState::Ambient);
        state_entry_ns_ = now_ns;
      }
      else if (now_ns - state_entry_ns_ >= (static_cast<std::uint64_t>(config_.activation_hold_ms) * 1'000'000ULL))
      {
        focus_target_ = input.track;
        transitionTo(ConversationState::Focused);
        state_entry_ns_ = now_ns;
      }
      break;
    case ConversationState::Focused:
      if (input.has_track && isAmbientZone(zone))
      {
        transitionTo(ConversationState::Releasing);
        state_entry_ns_ = now_ns;
      }
      else if (input.has_track && speech && directionStable(input.track.azimuth_deg))
      {
        focus_target_ = input.track;
      }
      else if (!input.has_track)
      {
        transitionTo(ConversationState::Held);
        state_entry_ns_ = now_ns;
      }
      else
      {
        transitionTo(ConversationState::Releasing);
        state_entry_ns_ = now_ns;
      }
      break;
    case ConversationState::Held:
      if (input.has_track && speech && isFocusEligible(zone))
      {
        focus_target_ = input.track;
        transitionTo(ConversationState::Focused);
        state_entry_ns_ = now_ns;
      }
      else if (now_ns - state_entry_ns_ >=
               (static_cast<std::uint64_t>(config_.hold_direction_ms) * 1'000'000ULL))
      {
        transitionTo(ConversationState::Releasing);
        state_entry_ns_ = now_ns;
      }
      break;
    case ConversationState::Releasing:
      if (input.has_track && speech && isFocusEligible(zone) &&
          (now_ns - state_entry_ns_ >=
           (static_cast<std::uint64_t>(config_.confirmation_hold_ms) * 1'000'000ULL)))
      {
        focus_target_ = input.track;
        transitionTo(ConversationState::Focused);
        state_entry_ns_ = now_ns;
      }
      else if (now_ns - state_entry_ns_ >=
               (static_cast<std::uint64_t>(config_.release_hold_ms) * 1'000'000ULL))
      {
        transitionTo(ConversationState::Ambient);
        state_entry_ns_ = now_ns;
      }
      break;
    default:
      break;
  }

  SteeringSnapshot out{};
  out.generation = ++generation_;
  out.zone_id = zone.has_value() ? static_cast<std::int16_t>(zone->index) : kNoZoneId;
  out.failsafe = (state_ == ConversationState::Ambient);
  out.confidence = input.confidence;
  out.speech_probability = input.speech_probability;
  if (state_ == ConversationState::Ambient)
  {
    out.target = {0.0F, 0.0F};
    out.ambient_mix = ambient_floor_linear_;
  }
  else
  {
    out.target = focus_target_;
    out.ambient_mix = 0.0F;
  }
  return out;
}

std::uint64_t ConversationStateMachine::transitionCount(const ConversationState from,
                                                        const ConversationState to) const
{
  return transitions_[Idx(from, to)];
}
}  // namespace sonitude::control
