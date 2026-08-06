#include "spatial/source_tracker.hpp"

namespace sonitude::spatial
{
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
    tr.obs.azimuth_deg = (0.75F * tr.obs.azimuth_deg) + (0.25F * obs.azimuth_deg);
    tr.obs.elevation_deg = (0.75F * tr.obs.elevation_deg) + (0.25F * obs.elevation_deg);
    tr.obs.confidence = obs.confidence;
    tr.obs.timestamp_ns = obs.timestamp_ns;
    tr.last_update_ns = now_ns;
  }

  for (auto it = tracks_.begin(); it != tracks_.end();)
  {
    if (now_ns - it->second.last_update_ns > stale_after_ns_)
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
  for (const auto& [id, track] : tracks_)
  {
    (void)id;
    if (now_ns - track.last_update_ns > stale_after_ns_)
    {
      continue;
    }
    if (best_track == nullptr || track.obs.confidence > best_track->obs.confidence)
    {
      best_track = &track;
    }
  }
  if (best_track == nullptr)
  {
    return std::nullopt;
  }
  return best_track->obs;
}
}  // namespace sonitude::spatial
