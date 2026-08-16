#include "dsp/own_voice_reference.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonitude::dsp
{
void OwnVoiceReferenceExtractor::configure(const OwnVoiceReferenceModel& model)
{
  if (!model.calibrated)
  {
    throw std::runtime_error("OwnVoiceReferenceExtractor requires a calibrated model");
  }

  float l1_norm = 0.0F;
  for (const float weight : model.weights)
  {
    if (!std::isfinite(weight))
    {
      throw std::runtime_error("own-voice reference weights must be finite");
    }
    l1_norm += std::fabs(weight);
  }
  if (!(l1_norm > 0.0F) || !std::isfinite(l1_norm))
  {
    throw std::runtime_error("own-voice reference weights must have non-zero finite norm");
  }

  // Unit L1 norm gives a deterministic gain bound for full-scale inputs while
  // preserving signed matched-filter weights.
  for (std::size_t channel = 0; channel < weights_.size(); ++channel)
  {
    weights_[channel] = model.weights[channel] / l1_norm;
  }
  configured_ = true;
}

bool OwnVoiceReferenceExtractor::process(const std::span<const audio::MicFrame> calibrated_input,
                                         const std::span<float> own_reference) const noexcept
{
  if (!configured_ || calibrated_input.size() != own_reference.size())
  {
    std::fill(own_reference.begin(), own_reference.end(), 0.0F);
    return false;
  }

  for (std::size_t frame_index = 0; frame_index < calibrated_input.size(); ++frame_index)
  {
    float sample = 0.0F;
    for (std::size_t channel = 0; channel < audio::kMicChannels; ++channel)
    {
      const float input = calibrated_input[frame_index][channel];
      if (!std::isfinite(input))
      {
        std::fill(own_reference.begin(), own_reference.end(), 0.0F);
        return false;
      }
      sample += weights_[channel] * input;
    }
    own_reference[frame_index] = sample;
  }
  return true;
}
} // namespace sonitude::dsp
