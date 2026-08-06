#include "dsp/calibration_applier.hpp"

#include <stdexcept>

namespace sonitude::dsp
{
CalibrationApplier::CalibrationApplier(const std::vector<app::CalibrationChannel>& channels)
{
  if (channels.size() != audio::kMicChannels)
  {
    throw std::runtime_error("CalibrationApplier expects six channels");
  }

  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    polarity_[i] = channels[i].polarity;
    gain_[i] = channels[i].gain_linear;
    dc_offset_[i] = channels[i].dc_offset;
  }
}

audio::MicFrame CalibrationApplier::process(const audio::MicFrame& in)
{
  // Lightweight RT-safe chain: polarity -> gain -> DC blocker (M3 baseline).
  audio::MicFrame out{};
  constexpr float hp_a = 0.995F;
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const float x = static_cast<float>(polarity_[i]) * (in[i] - dc_offset_[i]) * gain_[i];
    const float y = x - prev_x_[i] + (hp_a * prev_y_[i]);
    prev_x_[i] = x;
    prev_y_[i] = y;
    out[i] = y;
  }
  return out;
}
}  // namespace sonitude::dsp
