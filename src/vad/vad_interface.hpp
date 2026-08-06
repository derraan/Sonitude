#pragma once

#include <span>

namespace sonitude::vad
{
class IVad
{
public:
  virtual ~IVad() = default;
  virtual float EstimateSpeechProbability(std::span<const float> mono_frame) = 0;
};
}  // namespace sonitude::vad
