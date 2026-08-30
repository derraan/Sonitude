#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "dsp/spatial_looks.hpp"
#include "dsp/streaming_stft.hpp"

namespace sonitude::dsp
{
class IBeamformer
{
 public:
  virtual ~IBeamformer() = default;
  virtual void configure(const app::GeometryConfig& geometry,
                         const app::SteeringConfig& steering_config,
                         const app::CalibrationConfig& calibration,
                         std::uint32_t sample_rate_hz,
                         std::size_t max_block_frames) = 0;
  virtual void setTarget(audio::BeamformerSteering target) = 0;
  virtual void process(std::span<const audio::MicFrame> input, std::span<float> mono_out) = 0;
};

// Narrowband MVDR in the shared 128/32 STFT. Steering vector is the same
// far-field + calibration delay law previously used by delay-and-sum.
// Equal-weight delay-and-sum is the fallback when the covariance solve fails.
class MvdrBeamformer final : public IBeamformer
{
 public:
  void configure(const app::GeometryConfig& geometry,
                 const app::SteeringConfig& steering_config,
                 const app::CalibrationConfig& calibration,
                 std::uint32_t sample_rate_hz,
                 std::size_t max_block_frames) override;
  void setTarget(audio::BeamformerSteering target) override;
  void process(std::span<const audio::MicFrame> input, std::span<float> mono_out) override;
  void process(std::span<const audio::MicFrame> input,
               std::span<float> target_out,
               GuardLookSpans guards);
  [[nodiscard]] std::size_t algorithmicDelaySamples() const noexcept;

 private:
  using DelayArray = std::array<double, audio::kMicChannels>;
  using MicPosArray = std::array<std::array<double, 3>, audio::kMicChannels>;

  struct MicHopContext
  {
    MvdrBeamformer* self = nullptr;
    std::size_t channel = 0;
  };

  DelayArray computeRelativeDelays(audio::BeamformerSteering target) const;
  void updateGuardDelays();
  static void OnMicHop(void* context, float* re, float* im, std::size_t fft_size) noexcept;
  void StoreMicSpectrum(std::size_t channel, const float* re, const float* im, std::size_t fft_size) noexcept;
  void FormLooksAndSynthesize(std::size_t fft_size) noexcept;
  void FormLookSpectrum(const DelayArray& delays, std::vector<float>& y_re, std::vector<float>& y_im) const noexcept;
  void ApplyHermitian(std::vector<float>& y_re, std::vector<float>& y_im, std::size_t fft_size) const noexcept;

  bool configured_ = false;
  std::uint32_t sample_rate_hz_ = 0;
  std::size_t ramp_samples_ = 1;
  std::size_t fade_cursor_ = 0;
  bool crossfading_ = false;
  bool emit_guards_ = false;

  app::SteeringConfig steering_config_{};
  audio::BeamformerSteering current_target_{};
  MicPosArray mic_positions_{};
  DelayArray calibration_delays_{};
  DelayArray current_delays_{};
  DelayArray pending_delays_{};
  std::array<DelayArray, kGuardLooks> guard_delays_{};

  std::array<StreamingStft, audio::kMicChannels> mic_stft_{};
  std::array<MicHopContext, audio::kMicChannels> mic_ctx_{};
  StreamingStft target_stft_{};
  StreamingStft pending_stft_{};
  std::array<StreamingStft, kGuardLooks> guard_stft_{};

  std::array<std::vector<float>, audio::kMicChannels> x_re_{};
  std::array<std::vector<float>, audio::kMicChannels> x_im_{};
  std::vector<float> y_re_{};
  std::vector<float> y_im_{};
  std::vector<float> pending_y_re_{};
  std::vector<float> pending_y_im_{};
  std::array<std::vector<float>, kGuardLooks> guard_y_re_{};
  std::array<std::vector<float>, kGuardLooks> guard_y_im_{};

  // R[bin][row][col] packed as {re, im}.
  std::vector<std::array<std::array<std::array<float, 2>, audio::kMicChannels>, audio::kMicChannels>>
      cov_{};
  float cov_beta_ = 0.02F;
};

using DelaySumBeamformer = MvdrBeamformer;
}  // namespace sonitude::dsp
