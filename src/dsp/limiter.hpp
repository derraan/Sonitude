#pragma once

#include <cstdint>
#include <span>

#include "dsp/resampler.hpp"

namespace sonitude::dsp
{
struct LimiterConfig
{
  float ceiling_linear = 0.95F;
  float release_ms = 80.0F;
};

struct LimiterTelemetry
{
  std::uint32_t input_over_ceiling_events = 0;
  std::uint32_t output_saturation_events = 0;
};

class PeakLimiter
{
public:
  void configure(const LimiterConfig& config, std::uint32_t sample_rate_hz);
  void reset();
  LimiterTelemetry process(std::span<float> mono);
  LimiterTelemetry processLinkedStereo(std::span<StereoSample> stereo);
  float currentGain() const
  {
    return gain_;
  }

private:
  LimiterConfig config_{};
  bool configured_ = false;
  float gain_ = 1.0F;
  float release_step_per_sample_ = 0.01F;
};

class StereoPeakLimiter
{
public:
  void configure(const LimiterConfig& config, std::uint32_t sample_rate_hz);
  void reset();
  void process(std::span<float> left, std::span<float> right);
  float currentGain() const
  {
    return gain_;
  }

private:
  LimiterConfig config_{};
  bool configured_ = false;
  float gain_ = 1.0F;
  float release_step_per_sample_ = 0.01F;
};
} // namespace sonitude::dsp
