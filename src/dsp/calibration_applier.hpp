#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "app/calibration_config.hpp"
#include "audio/audio_types.hpp"

namespace sonitude::dsp
{
class CalibrationApplier
{
 public:
  explicit CalibrationApplier(const std::vector<app::CalibrationChannel>& channels);
  audio::MicFrame process(const audio::MicFrame& in);

 private:
  std::array<int, audio::kMicChannels> polarity_{};
  std::array<float, audio::kMicChannels> gain_{};
  std::array<float, audio::kMicChannels> dc_offset_{};
  std::array<float, audio::kMicChannels> prev_y_{};
  std::array<float, audio::kMicChannels> prev_x_{};
};
}  // namespace sonitude::dsp
