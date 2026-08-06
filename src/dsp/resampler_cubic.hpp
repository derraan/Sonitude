#pragma once

#include <memory>

#include "dsp/resampler.hpp"

namespace sonitude::dsp
{
std::unique_ptr<IStereoResampler> CreateCubicResampler();
}  // namespace sonitude::dsp
