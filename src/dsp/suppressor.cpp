#include "dsp/suppressor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonitude::dsp
{
namespace
{
float Clamp01(const float x)
{
  return std::clamp(x, 0.0F, 1.0F);
}
}  // namespace

void ConservativeSuppressor::configure(const SuppressorConfig& config, const std::uint32_t sample_rate_hz)
{
  if (sample_rate_hz == 0)
  {
    throw std::runtime_error("Suppressor sample_rate_hz must be non-zero");
  }

  config_ = config;
  config_.ambient_floor_linear = Clamp01(config_.ambient_floor_linear);
  config_.confidence_threshold = Clamp01(config_.confidence_threshold);
  config_.activity_threshold = std::max(0.0F, config_.activity_threshold);
  config_.fade_ms = std::max(1.0F, config_.fade_ms);
  config_.envelope_attack_coeff =
      std::clamp(config_.envelope_attack_coeff, 0.001F, 1.0F);
  config_.envelope_release_coeff =
      std::clamp(config_.envelope_release_coeff, 0.0001F, 1.0F);

  const float fade_samples =
      std::max(1.0F, (config_.fade_ms * 0.001F) * static_cast<float>(sample_rate_hz));
  gain_step_per_sample_ = 1.0F / fade_samples;

  envelope_attack_coeff_ = config_.envelope_attack_coeff;
  envelope_release_coeff_ = config_.envelope_release_coeff;

  focus_active_ = false;
  confidence_ = 0.0F;
  current_gain_ = 1.0F;
  envelope_ = 0.0F;
  configured_ = true;
}

void ConservativeSuppressor::setControl(const bool focus_active, const float confidence)
{
  focus_active_ = focus_active;
  confidence_ = Clamp01(confidence);
}

void ConservativeSuppressor::process(const std::span<float> mono)
{
  if (!configured_)
  {
    throw std::runtime_error("Suppressor used before configure");
  }

  for (float& sample : mono)
  {
    const float magnitude = std::fabs(sample);
    if (magnitude > envelope_)
    {
      envelope_ += envelope_attack_coeff_ * (magnitude - envelope_);
    }
    else
    {
      envelope_ += envelope_release_coeff_ * (magnitude - envelope_);
    }

    const bool allow_attenuation =
        focus_active_ && (confidence_ >= config_.confidence_threshold) &&
        (envelope_ >= config_.activity_threshold);
    const float target_gain = allow_attenuation ? config_.ambient_floor_linear : 1.0F;

    if (current_gain_ < target_gain)
    {
      current_gain_ = std::min(target_gain, current_gain_ + gain_step_per_sample_);
    }
    else
    {
      current_gain_ = std::max(target_gain, current_gain_ - gain_step_per_sample_);
    }
    current_gain_ = std::clamp(current_gain_, config_.ambient_floor_linear, 1.0F);
    sample *= current_gain_;
  }
}
}  // namespace sonitude::dsp
