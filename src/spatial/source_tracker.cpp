#include "spatial/source_tracker.hpp"

#include "spatial/angles.hpp"

namespace sonitude::spatial
{
namespace
{
bool IsStale(const std::uint64_t now_ns, const std::uint64_t last_update_ns, const std::uint64_t stale_after_ns)
{
  if (now_ns < last_update_ns)
  {
    return false;
  }
  return (now_ns - last_update_ns) > stale_after_ns;
}

bool BetterCandidate(const SourceObservation& candidate_obs,
                     const std::uint64_t candidate_last_update_ns,
                     const std::uint64_t candidate_id,
                     const SourceObservation& current_best_obs,
                     const std::uint64_t current_best_last_update_ns,
                     const std::uint64_t current_best_id)
{
  if (candidate_obs.confidence != current_best_obs.confidence)
  {
    return candidate_obs.confidence > current_best_obs.confidence;
  }
  if (candidate_last_update_ns != current_best_last_update_ns)
  {
    return candidate_last_update_ns > current_best_last_update_ns;
  }
  return candidate_id < current_best_id;
}
}  // namespace

void SourceTracker::ingest(const std::vector<SourceObservation>& observations, const std::uint64_t now_ns)
{
  for (const SourceObservation& obs : observations)
  {
    auto& tr = tracks_[obs.source_id];
    if (tr.last_update_ns == 0)
    {
      tr.obs = obs;
      tr.last_update_ns = now_ns;
      continue;
    }
    const double delta =
        SignedAngularDistanceDeg(static_cast<double>(tr.obs.azimuth_deg), static_cast<double>(obs.azimuth_deg));
    tr.obs.azimuth_deg = static_cast<float>(
        NormalizeAzimuthDeg(static_cast<double>(tr.obs.azimuth_deg) + (0.25 * delta)));
    tr.obs.elevation_deg = (0.75F * tr.obs.elevation_deg) + (0.25F * obs.elevation_deg);
    tr.obs.confidence = obs.confidence;
    tr.obs.timestamp_ns = obs.timestamp_ns;
    tr.last_update_ns = now_ns;
  }

  for (auto it = tracks_.begin(); it != tracks_.end();)
  {
    if (IsStale(now_ns, it->second.last_update_ns, stale_after_ns_))
    {
      it = tracks_.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

std::optional<SourceObservation> SourceTracker::best(const std::uint64_t now_ns) const
{
  const Track* best_track = nullptr;
  std::uint64_t best_id = 0;
  for (const auto& [id, track] : tracks_)
  {
    if (IsStale(now_ns, track.last_update_ns, stale_after_ns_))
    {
      continue;
    }
    if (best_track == nullptr ||
        BetterCandidate(track.obs,
                        track.last_update_ns,
                        id,
                        best_track->obs,
                        best_track->last_update_ns,
                        best_id))
    {
      best_track = &track;
      best_id = id;
    }
  }
  if (best_track == nullptr)
  {
    return std::nullopt;
  }
  return best_track->obs;
}
}  // namespace sonitude::spatial
