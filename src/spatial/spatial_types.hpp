#pragma once

#include <cstdint>

namespace sonitude::spatial
{
struct SourceObservation
{
  std::uint64_t source_id = 0;
  float azimuth_deg = 0.0F;
  float elevation_deg = 0.0F;
  float confidence = 0.0F;
  std::uint64_t timestamp_ns = 0;
};
}  // namespace sonitude::spatial
