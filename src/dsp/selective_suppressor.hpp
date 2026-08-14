#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sonitude::dsp
{
struct SelectiveSuppressorConfig
{
  std::size_t taps = 24;
  float step_size = 0.35F;
  float leakage = 0.0005F;
  float max_attenuation_db = 12.0F;
};

class SelectiveSuppressor
{
public:
  void configure(const SelectiveSuppressorConfig& config, std::uint32_t sample_rate_hz);
  void setControl(bool enabled, bool has_distractor);
  void process(std::span<float> focus_signal, std::span<const float> distractor_reference);

private:
  static float Clamp(float value, float min_value, float max_value);

  bool configured_ = false;
  bool enabled_ = false;
  bool has_distractor_ = false;
  SelectiveSuppressorConfig config_{};
  std::vector<float> weights_{};
  std::vector<float> history_{};
};
} // namespace sonitude::dsp
