#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"

namespace sonitude::app
{
struct CalibrationChannelReport
{
  std::string id;
  int polarity = 1;
  bool polarity_unresolved = false;
  float gain_linear = 1.0F;
  float relative_gain_db = 0.0F;
  float delay_samples = 0.0F;
  float delay_us = 0.0F;
  float dc_offset = 0.0F;
  float correlation_peak = 0.0F;
  float delay_confidence = 0.0F;
  float rms_level = 0.0F;
  bool clipping = false;
  CalibrationQualityStatus status = CalibrationQualityStatus::Pass;
  std::vector<std::string> warnings;
};

struct CalibrationEstimateReport
{
  std::uint32_t sample_rate_hz = 0;
  std::size_t reference_channel_index = 0;
  std::string reference_microphone_id;
  std::size_t silence_frame_count = 0;
  std::size_t signal_frame_count = 0;
  bool hardware_evidence = false;
  CalibrationQualityStatus overall_status = CalibrationQualityStatus::Invalid;
  std::vector<CalibrationChannelReport> channels;
  std::vector<std::string> warnings;
};

void WriteCalibrationYamlBackupSafe(const std::string& path,
                                    const CalibrationConfig& calibration,
                                    bool force_overwrite);
void WriteCalibrationReport(const std::string& path, const CalibrationEstimateReport& report);
}  // namespace sonitude::app
