#pragma once

#include <memory>

#include "dsp/resampler.hpp"
#include "dsp/resampler_linear.hpp"

namespace sonitude::dsp
{
inline std::unique_ptr<IStereoResampler> CreateCubicResampler()
{
  return CreateLinearResampler();
}
}  // namespace sonitude::dsp
