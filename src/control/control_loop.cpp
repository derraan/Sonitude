#include "control/control_loop.hpp"

#include <algorithm>
#include <optional>

#include "spatial/mock_doa_provider.hpp"

namespace sonitude::control
{
namespace
{
bool TimeoutElapsed(const std::uint64_t now_ns,
                    const std::uint64_t start_ns,
                    const std::uint64_t timeout_ns)
{
  if (now_ns < start_ns)
  {
    return false;
  }
  return (now_ns - start_ns) >= timeout_ns;
}
}  // namespace

ControlLoop::ControlLoop(spatial::IDoaProvider* provider,
                         rt::SnapshotBuffer<SteeringSnapshot>& snapshots,
                         const ControlLoopConfig config,
                         ConversationStateMachine* conversation)
    : provider_(provider),
      snapshots_(&snapshots),
      config_(config),
      conversation_(conversation),
      tracker_(config.failsafe_timeout_ns),
      last_observation_ns_(0),
      generation_(0)
{
  last_snapshot_.ambient_mix = config_.ambient_floor_linear;
  last_snapshot_.failsafe = true;
  last_snapshot_.generation = generation_;
  snapshots_->publish(last_snapshot_);
}

void ControlLoop::tick(const std::uint64_t now_ns)
{
  if (auto* mock = dynamic_cast<spatial::MockDoaProvider*>(provider_))
  {
    mock->setNowNs(now_ns);
  }

  std::vector<spatial::SourceObservation> observations;
  const bool got_data = provider_->poll(observations);
  if (got_data && !observations.empty())
  {
    tracker_.ingest(observations, now_ns);
    last_observation_ns_ = now_ns;
  }

  const bool provider_healthy = provider_->isHealthy();
  const bool stale_observations =
      TimeoutElapsed(now_ns, last_observation_ns_, config_.failsafe_timeout_ns);
  const std::optional<spatial::SourceObservation> active =
      (!provider_healthy || stale_observations) ? std::nullopt : tracker_.best(now_ns);
  SteeringSnapshot next{};
  if (conversation_ != nullptr)
  {
    ConversationInput input{};
    if (active.has_value())
    {
      input.has_track = true;
      input.track.azimuth_deg = active->azimuth_deg;
      input.track.elevation_deg = active->elevation_deg;
      input.confidence = active->confidence;
      // ODAS activity remains a proxy until a production VAD is integrated.
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
    next.confidence = active->confidence;
    next.speech_probability = active->confidence;
    next.failsafe = false;
  }
  else if (stale_observations || !provider_healthy)
  {
    next = last_snapshot_;
    ++generation_;
    next.generation = generation_;
    next.target = {0.0F, 0.0F};
    next.ambient_mix = config_.ambient_floor_linear;
    next.confidence = 0.0F;
    next.speech_probability = 0.0F;
    next.zone_id = kNoZoneId;
    next.failsafe = true;
  }
  else
  {
    next = last_snapshot_;
    ++generation_;
    next.generation = generation_;
  }

  if (stale_observations || !provider_healthy)
  {
    next.target = {0.0F, 0.0F};
    next.ambient_mix = config_.ambient_floor_linear;
    next.confidence = 0.0F;
    next.speech_probability = 0.0F;
    next.zone_id = kNoZoneId;
    next.failsafe = true;
  }
  else if (conversation_ == nullptr)
  {
    next.confidence = active.has_value() ? std::clamp(active->confidence, 0.0F, 1.0F) : 0.0F;
  }

  last_snapshot_ = next;
  snapshots_->publish(next);
}
}  // namespace sonitude::control
