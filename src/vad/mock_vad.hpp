#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "vad/vad_interface.hpp"

namespace sonitude::vad
{
class MockVad final : public IVad
{
 public:
  // TODO(sonitude-vad): Replace scripted probabilities with a production VAD backend that
  // implements IVad and provides calibrated speech probability estimates.
  explicit MockVad(std::vector<float> scripted_probabilities)
      : scripted_probabilities_(std::move(scripted_probabilities))
  {
    if (scripted_probabilities_.empty())
    {
      scripted_probabilities_.push_back(0.0F);
    }
  }

  float EstimateSpeechProbability(std::span<const float> mono_frame) override
  {
    (void)mono_frame;
    const std::size_t index = std::min(cursor_, scripted_probabilities_.size() - 1U);
    const float out = scripted_probabilities_[index];
    ++cursor_;
    return out;
  }

  void reset()
  {
    cursor_ = 0;
  }

 private:
  std::vector<float> scripted_probabilities_;
  std::size_t cursor_ = 0;
};
}  // namespace sonitude::vad
