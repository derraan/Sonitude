#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sonitude::app
{
struct CalibrationChannel
{
  std::string id;
  int polarity = 1;
  float gain_linear = 1.0F;
  float delay_samples = 0.0F;
  float dc_offset = 0.0F;
};

struct CalibrationConfig
{
  std::uint32_t sample_rate_hz = 0;
  std::vector<CalibrationChannel> channels;
};

CalibrationConfig LoadCalibrationFromFile(const std::string& path);
void ValidateCalibrationConfig(const CalibrationConfig& calibration,
                               const std::vector<std::string>& geometry_ids,
                               std::uint32_t expected_sample_rate_hz);
}  // namespace sonitude::app
