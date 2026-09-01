#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sonitude::app
{
constexpr int kCalibrationSchemaVersion = 2;

enum class CalibrationQualityStatus
{
  Pass,
  Warning,
  Unresolved,
  Invalid
};

struct CalibrationChannel
{
  std::string id;
  int polarity = 1;
  float gain_linear = 1.0F;
  float delay_samples = 0.0F;
  float dc_offset = 0.0F;
};

struct CalibrationReference
{
  std::string microphone_id;
};

struct CalibrationIdentity
{
  std::string geometry_id;
  std::string device_id;
  std::string calibration_sequence;
  std::string created_utc;
};

struct CalibrationCaptureInfo
{
  std::uint32_t sample_rate_hz = 0;
  std::size_t channel_count = 0;
};

struct CalibrationQuality
{
  bool valid = false;
  bool hardware_evidence = false;
  std::vector<std::string> warnings;
};

struct CalibrationConfig
{
  int schema_version = 1;
  std::uint32_t sample_rate_hz = 0;
  CalibrationReference reference;
  CalibrationIdentity identity;
  CalibrationCaptureInfo capture;
  std::vector<CalibrationChannel> channels;
  CalibrationQuality quality;
};

CalibrationConfig LoadCalibrationFromFile(const std::string& path);
void ValidateCalibrationConfig(const CalibrationConfig& calibration,
                               const std::vector<std::string>& geometry_ids,
                               std::uint32_t expected_sample_rate_hz);
const char* CalibrationQualityStatusToString(CalibrationQualityStatus status) noexcept;
}  // namespace sonitude::app
