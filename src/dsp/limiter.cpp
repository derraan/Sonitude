#include "dsp/limiter.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonitude::dsp
{
namespace
{
float UpdateGain(const float amplitude,
                 const float ceiling_linear,
                 const float release_step_per_sample,
                 const float current_gain)
{
  float target_gain = 1.0F;
  if (amplitude > ceiling_linear)
  {
    target_gain = ceiling_linear / amplitude;
  }
  if (target_gain < current_gain)
  {
    return target_gain;
  }
  return std::min(1.0F, current_gain + release_step_per_sample);
}
}  // namespace

void PeakLimiter::configure(const LimiterConfig& config, const std::uint32_t sample_rate_hz)
{
  if (sample_rate_hz == 0)
  {
    throw std::runtime_error("Limiter sample_rate_hz must be non-zero");
  }

  config_ = config;
  config_.ceiling_linear = std::clamp(config_.ceiling_linear, 0.1F, 1.0F);
  config_.release_ms = std::max(1.0F, config_.release_ms);

  const float release_samples =
      std::max(1.0F, (config_.release_ms * 0.001F) * static_cast<float>(sample_rate_hz));
  release_step_per_sample_ = 1.0F / release_samples;
  gain_ = 1.0F;
  configured_ = true;
}

void PeakLimiter::reset()
{
  gain_ = 1.0F;
}

LimiterTelemetry PeakLimiter::process(const std::span<float> mono)
{
  if (!configured_)
  {
    throw std::runtime_error("Limiter used before configure");
  }

  LimiterTelemetry telemetry{};
  for (float& sample : mono)
  {
    const float amplitude = std::fabs(sample);
    if (amplitude > config_.ceiling_linear)
    {
      ++telemetry.input_over_ceiling_events;
    }
    gain_ = UpdateGain(amplitude, config_.ceiling_linear, release_step_per_sample_, gain_);
    sample *= gain_;
    if (std::fabs(sample) > 1.0F)
    {
      ++telemetry.output_saturation_events;
    }
  }
  return telemetry;
}

LimiterTelemetry PeakLimiter::processLinkedStereo(const std::span<StereoSample> stereo)
{
  if (!configured_)
  {
    throw std::runtime_error("Limiter used before configure");
  }

  LimiterTelemetry telemetry{};
  for (StereoSample& sample : stereo)
  {
    const float frame_peak = std::max(std::fabs(sample.left), std::fabs(sample.right));
    if (frame_peak > config_.ceiling_linear)
    {
      ++telemetry.input_over_ceiling_events;
    }
    gain_ = UpdateGain(frame_peak, config_.ceiling_linear, release_step_per_sample_, gain_);
    sample.left *= gain_;
    sample.right *= gain_;
    if (std::fabs(sample.left) > 1.0F)
    {
      ++telemetry.output_saturation_events;
    }
    if (std::fabs(sample.right) > 1.0F)
    {
      ++telemetry.output_saturation_events;
    }
  }
  return telemetry;
}

void StereoPeakLimiter::configure(const LimiterConfig& config, const std::uint32_t sample_rate_hz)
{
  if (sample_rate_hz == 0)
  {
    throw std::runtime_error("Stereo limiter sample_rate_hz must be non-zero");
  }

  config_ = config;
  config_.ceiling_linear = std::clamp(config_.ceiling_linear, 0.1F, 1.0F);
  config_.release_ms = std::max(1.0F, config_.release_ms);

  const float release_samples =
      std::max(1.0F, (config_.release_ms * 0.001F) * static_cast<float>(sample_rate_hz));
  release_step_per_sample_ = 1.0F / release_samples;
  gain_ = 1.0F;
  configured_ = true;
}

void StereoPeakLimiter::reset()
{
  gain_ = 1.0F;
}

void StereoPeakLimiter::process(const std::span<float> left, const std::span<float> right)
{
  if (!configured_)
  {
    throw std::runtime_error("Stereo limiter used before configure");
  }
  if (left.size() != right.size())
  {
    throw std::runtime_error("Stereo limiter requires equal left/right span sizes");
  }

  for (std::size_t i = 0; i < left.size(); ++i)
  {
    float l = left[i];
    float r = right[i];
    if (!std::isfinite(l))
    {
      l = 0.0F;
    }
    if (!std::isfinite(r))
    {
      r = 0.0F;
    }
    const float amplitude = std::max(std::fabs(l), std::fabs(r));
    gain_ = UpdateGain(amplitude, config_.ceiling_linear, release_step_per_sample_, gain_);
    left[i] = l * gain_;
    right[i] = r * gain_;
  }
}
}  // namespace sonitude::dsp
