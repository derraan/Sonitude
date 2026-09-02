#include "dsp/selective_suppressor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonitude::dsp
{
namespace
{
constexpr float kEpsilon = 1e-6F;
}

float SelectiveSuppressor::Clamp(const float value, const float min_value, const float max_value)
{
  return std::max(min_value, std::min(value, max_value));
}

void SelectiveSuppressor::configure(const SelectiveSuppressorConfig& config,
                                    const std::uint32_t sample_rate_hz)
{
  if (sample_rate_hz == 0)
  {
    throw std::runtime_error("SelectiveSuppressor sample_rate_hz must be non-zero");
  }
  config_ = config;
  if (config_.taps == 0)
  {
    config_.taps = 1;
  }
  config_.step_size = Clamp(config_.step_size, 0.001F, 1.0F);
  config_.leakage = Clamp(config_.leakage, 0.0F, 0.1F);
  config_.max_attenuation_db = Clamp(config_.max_attenuation_db, 0.0F, 24.0F);

  weights_.assign(config_.taps, 0.0F);
  history_.assign(config_.taps, 0.0F);
  configured_ = true;
  enabled_ = false;
  has_distractor_ = false;
}

void SelectiveSuppressor::setControl(const bool enabled, const bool has_distractor)
{
  enabled_ = enabled;
  has_distractor_ = has_distractor;
}

void SelectiveSuppressor::process(const std::span<float> focus_signal,
                                  const std::span<const float> distractor_reference)
{
  if (!configured_)
  {
    throw std::runtime_error("SelectiveSuppressor used before configure");
  }
  if (focus_signal.size() != distractor_reference.size())
  {
    throw std::runtime_error("SelectiveSuppressor expects equal-sized focus/reference spans");
  }

  const bool adapt = enabled_ && has_distractor_;
  const float floor_gain = std::pow(10.0F, -config_.max_attenuation_db / 20.0F);

  for (std::size_t n = 0; n < focus_signal.size(); ++n)
  {
    for (std::size_t i = history_.size() - 1; i > 0; --i)
    {
      history_[i] = history_[i - 1];
    }
    history_[0] = distractor_reference[n];

    float estimated = 0.0F;
    float norm = kEpsilon;
    for (std::size_t i = 0; i < weights_.size(); ++i)
    {
      estimated += weights_[i] * history_[i];
      norm += history_[i] * history_[i];
    }

    const float input = focus_signal[n];
    float error = input - estimated;
    if (std::fabs(input) > kEpsilon)
    {
      const float min_abs = floor_gain * std::fabs(input);
      const float error_sign = (error < 0.0F) ? -1.0F : 1.0F;
      error = error_sign * std::max(min_abs, std::fabs(error));
    }

    if (adapt)
    {
      const float mu = config_.step_size / norm;
      for (std::size_t i = 0; i < weights_.size(); ++i)
      {
        weights_[i] = ((1.0F - config_.leakage) * weights_[i]) + (mu * error * history_[i]);
      }
    }
    else
    {
      for (float& w : weights_)
      {
        w *= (1.0F - config_.leakage);
      }
    }

    focus_signal[n] = error;
  }
}
} // namespace sonitude::dsp
