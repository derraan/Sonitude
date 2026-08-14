#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "dsp/fractional_delay.hpp"

namespace sonitude::dsp
{
class IBeamformer
{
public:
  virtual ~IBeamformer() = default;
  virtual void configure(const app::GeometryConfig& geometry,
                         const app::SteeringConfig& steering_config,
                         const app::CalibrationConfig& calibration, std::uint32_t sample_rate_hz,
                         std::size_t max_block_frames) = 0;
  virtual void setTarget(audio::BeamformerSteering target) = 0;
  virtual void process(std::span<const audio::MicFrame> input, std::span<float> mono_out) = 0;
};

class DelaySumBeamformer final : public IBeamformer
{
public:
  void configure(const app::GeometryConfig& geometry, const app::SteeringConfig& steering_config,
                 const app::CalibrationConfig& calibration, std::uint32_t sample_rate_hz,
                 std::size_t max_block_frames) override;
  void setTarget(audio::BeamformerSteering target) override;
  void process(std::span<const audio::MicFrame> input, std::span<float> mono_out) override;
  void processWithReference(std::span<const audio::MicFrame> input, std::span<float> focus_out,
                            audio::BeamformerSteering distractor_target,
                            std::span<float> reference_out);

private:
  using DelayArray = std::array<double, audio::kMicChannels>;
  using MicPosArray = std::array<std::array<double, 3>, audio::kMicChannels>;

  DelayArray computeDelaysForTarget(audio::BeamformerSteering target) const;
  float renderOne(const audio::MicFrame& frame, const DelayArray& delays,
                  std::array<FractionalDelayLine, audio::kMicChannels>& lines) const;

  bool configured_ = false;
  std::uint32_t sample_rate_hz_ = 0;
  std::size_t ramp_samples_ = 1;
  std::size_t fade_cursor_ = 0;
  bool crossfading_ = false;

  app::SteeringConfig steering_config_{};
  audio::BeamformerSteering current_target_{};
  MicPosArray mic_positions_{};
  DelayArray calibration_delays_{};
  DelayArray current_delays_{};
  DelayArray pending_delays_{};
  double max_aperture_delay_samples_ = 0.0;
  double max_calibration_abs_delay_ = 0.0;
  double base_delay_samples_ = 0.0;

  std::array<FractionalDelayLine, audio::kMicChannels> current_lines_{};
  std::array<FractionalDelayLine, audio::kMicChannels> pending_lines_{};
  std::array<FractionalDelayLine, audio::kMicChannels> reference_lines_{};
};
} // namespace sonitude::dsp
