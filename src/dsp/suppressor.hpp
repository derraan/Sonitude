#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sonitude::dsp
{
struct SuppressorConfig
{
  float ambient_floor_linear = 0.25F;
  float fade_ms = 120.0F;
  float activity_threshold = 0.03F;
  float confidence_threshold = 0.6F;
};

class ISuppressor
{
 public:
  virtual ~ISuppressor() = default;
  virtual void configure(const SuppressorConfig& config, std::uint32_t sample_rate_hz) = 0;
  virtual void setControl(bool focus_active, float confidence) = 0;
  virtual void process(std::span<float> mono) = 0;
  virtual float currentGain() const = 0;
};

class OutputGainGate final : public ISuppressor
{
 public:
  void configure(const SuppressorConfig& config, std::uint32_t sample_rate_hz) override;
  void setControl(bool focus_active, float confidence) override;
  void process(std::span<float> mono) override;
  float currentGain() const override { return current_gain_; }

 private:
  SuppressorConfig config_{};
  bool configured_ = false;
  bool focus_active_ = false;
  float confidence_ = 0.0F;
  float current_gain_ = 1.0F;
  float envelope_ = 0.0F;
  float gain_step_per_sample_ = 1.0F;
  float envelope_attack_coeff_ = 0.35F;
  float envelope_release_coeff_ = 0.01F;
};

// TODO(sonitude-suppression): Remove legacy alias once all downstream references are migrated.
using ConservativeSuppressor = OutputGainGate;
}  // namespace sonitude::dsp
