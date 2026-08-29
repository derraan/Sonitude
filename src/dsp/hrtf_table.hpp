#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonitude::dsp
{
struct HrtfDirection
{
  float azimuth_deg = 0.0F;
  float elevation_deg = 0.0F;
  float delay_left_samples = 0.0F;
  float delay_right_samples = 0.0F;
};

struct HrtfTable
{
  std::uint32_t sample_rate_hz = 0;
  std::uint32_t taps_per_ear = 0;
  std::vector<HrtfDirection> directions;
  std::vector<float> fir;  // [direction][ear][tap], ear: 0=left, 1=right

  std::size_t directionCount() const { return directions.size(); }
  bool empty() const { return directions.empty() || taps_per_ear == 0 || fir.empty(); }
};

HrtfTable LoadHrtfTableFromFile(const std::string& path);
}  // namespace sonitude::dsp
