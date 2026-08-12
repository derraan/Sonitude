#pragma once

#include <memory>

#include "dsp/resampler.hpp"

namespace sonitude::dsp
{
std::unique_ptr<IStereoResampler> CreateLinearResampler();
}  // namespace sonitude::dsp
