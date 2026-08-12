#include "dsp/calibration_applier.hpp"

#include <cmath>
#include <stdexcept>

namespace sonitude::dsp
{
CalibrationApplier::CalibrationApplier(const std::vector<app::CalibrationChannel>& channels,
                                       const std::uint32_t sample_rate_hz,
                                       const float dc_block_hz)
{
  if (channels.size() != audio::kMicChannels)
  {
    throw std::runtime_error("CalibrationApplier expects six channels");
  }
  if (sample_rate_hz == 0 || dc_block_hz <= 0.0F)
  {
    throw std::runtime_error("CalibrationApplier requires a non-zero sample rate and positive dc_block_hz");
  }

  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    polarity_[i] = channels[i].polarity;
    gain_[i] = channels[i].gain_linear;
    dc_offset_[i] = channels[i].dc_offset;
  }
  const double alpha = std::exp((-2.0 * 3.14159265358979323846 * static_cast<double>(dc_block_hz)) /
                                static_cast<double>(sample_rate_hz));
  hp_a_ = static_cast<float>(alpha);
}

audio::MicFrame CalibrationApplier::process(const audio::MicFrame& in)
{
  // Lightweight RT-safe chain: polarity -> DC subtract -> gain -> DC blocker.
  audio::MicFrame out{};
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const float x = static_cast<float>(polarity_[i]) * (in[i] - dc_offset_[i]) * gain_[i];
    const float y = x - prev_x_[i] + (hp_a_ * prev_y_[i]);
    prev_x_[i] = x;
    prev_y_[i] = y;
    out[i] = y;
  }
  return out;
}

void CalibrationApplier::processBlock(std::span<const audio::MicFrame> in, std::span<audio::MicFrame> out)
{
  if (out.size() < in.size())
  {
    throw std::runtime_error("CalibrationApplier output span is smaller than input span");
  }
  for (std::size_t i = 0; i < in.size(); ++i)
  {
    out[i] = process(in[i]);
  }
}
}  // namespace sonitude::dsp
