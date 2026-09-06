#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/calibration_writer.hpp"

namespace sonitude::app
{
struct CalibrationEstimateOptions
{
  std::vector<std::string> channel_ids;
  std::size_t reference_channel_index = 0;
  std::uint32_t sample_rate_hz = 44100;
  std::size_t silence_start_frame = 0;
  std::size_t silence_frame_count = 0;
  std::size_t signal_start_frame = 0;
  std::size_t signal_frame_count = 0;
  std::string geometry_id;
  bool hardware_evidence = false;
  float clipping_threshold = 0.99F;
  float min_signal_rms = 1.0e-4F;
  float polarity_correlation_threshold = 0.2F;
  float delay_confidence_threshold = 0.15F;
};

struct CalibrationEstimateOutput
{
  CalibrationConfig calibration;
  CalibrationEstimateReport report;
};

CalibrationEstimateOutput EstimateCalibrationFromCapture(
    std::span<const float> interleaved,
    std::size_t channel_count,
    const CalibrationEstimateOptions& options);
}  // namespace sonitude::app
