#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "spatial/spatial_types.hpp"

namespace sonitude::spatial
{
class SourceTracker
{
public:
  explicit SourceTracker(std::uint64_t stale_after_ns) : stale_after_ns_(stale_after_ns) {}

  void ingest(const std::vector<SourceObservation>& observations, std::uint64_t now_ns);
  std::optional<SourceObservation> best(std::uint64_t now_ns) const;
  std::optional<SourceObservation> strongestDistractor(std::uint64_t now_ns,
                                                       std::uint64_t focus_source_id) const;

private:
  struct Track
  {
    SourceObservation obs{};
    std::uint64_t last_update_ns = 0;
  };

  std::unordered_map<std::uint64_t, Track> tracks_;
  std::uint64_t stale_after_ns_ = 1'500'000'000ULL;
};
} // namespace sonitude::spatial
