#pragma once

#include <cstdint>
#include <span>

namespace sonitude::dsp
{
struct LimiterConfig
{
  float ceiling_linear = 0.95F;
  float release_ms = 80.0F;
};

class PeakLimiter
{
 public:
  void configure(const LimiterConfig& config, std::uint32_t sample_rate_hz);
  void reset();
  void process(std::span<float> mono);
  float currentGain() const { return gain_; }

 private:
  LimiterConfig config_{};
  bool configured_ = false;
  float gain_ = 1.0F;
  float release_step_per_sample_ = 0.01F;
};

class StereoPeakLimiter
{
 public:
  // Linked stereo sample-peak limiter: one shared gain from max(|L|,|R|) per
  // sample. No look-ahead and no inter-sample/true-peak detection.
  void configure(const LimiterConfig& config, std::uint32_t sample_rate_hz);
  void reset();
  void process(std::span<float> left, std::span<float> right);
  float currentGain() const { return gain_; }

 private:
  LimiterConfig config_{};
  bool configured_ = false;
  float gain_ = 1.0F;
  float release_step_per_sample_ = 0.01F;
};
}  // namespace sonitude::dsp
