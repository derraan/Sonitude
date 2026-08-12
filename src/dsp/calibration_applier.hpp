#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "app/calibration_config.hpp"
#include "audio/audio_types.hpp"

namespace sonitude::dsp
{
class CalibrationApplier
{
 public:
  CalibrationApplier(const std::vector<app::CalibrationChannel>& channels,
                     std::uint32_t sample_rate_hz,
                     float dc_block_hz);
  audio::MicFrame process(const audio::MicFrame& in);
  void processBlock(std::span<const audio::MicFrame> in, std::span<audio::MicFrame> out);

 private:
  std::array<int, audio::kMicChannels> polarity_{};
  std::array<float, audio::kMicChannels> gain_{};
  std::array<float, audio::kMicChannels> dc_offset_{};
  std::array<float, audio::kMicChannels> prev_y_{};
  std::array<float, audio::kMicChannels> prev_x_{};
  float hp_a_ = 0.0F;
};
}  // namespace sonitude::dsp
