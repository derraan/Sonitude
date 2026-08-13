#pragma once

#include <memory>

#include "dsp/resampler.hpp"
#include "dsp/resampler_linear.hpp"

namespace sonitude::dsp
{
inline std::unique_ptr<IStereoResampler> CreateCubicResampler()
{
  // TODO(sonitude-resampler): Implement a true cubic resampler and retire this linear fallback
  // once quality/performance parity tests are in place.
  return CreateLinearResampler();
}
}  // namespace sonitude::dsp
