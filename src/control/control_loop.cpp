#include "control/control_loop.hpp"

#include <optional>

#include "spatial/mock_doa_provider.hpp"

namespace sonitude::control
{
ControlLoop::ControlLoop(spatial::IDoaProvider* provider,
                         SteeringChannel* channel,
                         const ControlLoopConfig config,
                         ConversationStateMachine* conversation)
    : provider_(provider),
      channel_(channel),
      config_(config),
      conversation_(conversation),
      tracker_(config.failsafe_timeout_ns),
      last_observation_ns_(0),
      generation_(0)
{
  observations_.reserve(16);
  last_snapshot_.ambient_mix = config_.ambient_floor_linear;
  last_snapshot_.failsafe = true;
  last_snapshot_.generation = generation_;
  publish(last_snapshot_, 0);
}

void ControlLoop::publish(const SteeringSnapshot& snapshot, const std::uint64_t now_ns)
{
  const std::uint8_t state =
      conversation_ != nullptr ? static_cast<std::uint8_t>(conversation_->state()) : 0U;
  (void)channel_->publish(ToRtSnapshot(snapshot, now_ns, state));
}

void ControlLoop::publishFailsafe(const std::uint64_t now_ns)
{
  SteeringSnapshot safe = last_snapshot_;
  ++generation_;
  safe.generation = generation_;
  safe.target = {0.0F, 0.0F};
  safe.distractor = {0.0F, 0.0F};
  safe.has_distractor = false;
  safe.ambient_mix = config_.ambient_floor_linear;
  safe.failsafe = true;
  last_snapshot_ = safe;
  publish(safe, now_ns);
}

void ControlLoop::tick(const std::uint64_t now_ns)
{
  if (auto* mock = dynamic_cast<spatial::MockDoaProvider*>(provider_))
  {
    mock->setNowNs(now_ns);
  }

  observations_.clear();
  const bool got_data = provider_->poll(observations_);
  if (got_data && !observations_.empty())
  {
    tracker_.ingest(observations_, now_ns);
    last_observation_ns_ = now_ns;
  }

  const std::optional<spatial::SourceObservation> active = tracker_.best(now_ns);
  SteeringSnapshot next{};
  if (conversation_ != nullptr)
  {
    ConversationInput input{};
    if (active.has_value())
    {
      input.has_track = true;
      input.track.azimuth_deg = active->azimuth_deg;
      input.track.elevation_deg = active->elevation_deg;
      input.speech_probability = active->confidence;
    }
    next = conversation_->update(input, now_ns);
  }
  else if (active.has_value())
  {
    next = last_snapshot_;
    ++generation_;
    next.generation = generation_;
    next.target.azimuth_deg = active->azimuth_deg;
    next.target.elevation_deg = active->elevation_deg;
    next.ambient_mix = 0.0F;
    next.failsafe = false;
  }
  else if (now_ns - last_observation_ns_ >= config_.failsafe_timeout_ns || !provider_->isHealthy())
  {
    next = last_snapshot_;
    ++generation_;
    next.generation = generation_;
    next.target = {0.0F, 0.0F};
    next.ambient_mix = config_.ambient_floor_linear;
    next.failsafe = true;
  }
  else
  {
    next = last_snapshot_;
    ++generation_;
    next.generation = generation_;
  }

  last_snapshot_ = next;
  publish(next, now_ns);
}
}  // namespace sonitude::control
