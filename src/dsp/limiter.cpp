#include "dsp/limiter.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonitude::dsp
{
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
    float target_gain = 1.0F;
    if (amplitude > config_.ceiling_linear)
    {
      ++telemetry.input_over_ceiling_events;
      target_gain = config_.ceiling_linear / amplitude;
    }

    if (target_gain < gain_)
    {
      gain_ = target_gain;
    }
    else
    {
      gain_ = std::min(1.0F, gain_ + release_step_per_sample_);
    }

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
    float target_gain = 1.0F;
    if (frame_peak > config_.ceiling_linear)
    {
      ++telemetry.input_over_ceiling_events;
      target_gain = config_.ceiling_linear / frame_peak;
    }

    if (target_gain < gain_)
    {
      gain_ = target_gain;
    }
    else
    {
      gain_ = std::min(1.0F, gain_ + release_step_per_sample_);
    }

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
}  // namespace sonitude::dsp
