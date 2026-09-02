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
#include "dsp/steering_lut.hpp"
#include "dsp/streaming_stft.hpp"

namespace sonitude::dsp
{
class SpectralPostfilter;

struct MvdrTuningParams
{
  float diag_load = 0.08F;
  float max_white_noise_gain = 4.0F;
  float cov_tau_sec = 0.080F;
};

class MvdrBeamformer
{
 public:
  void configure(const app::GeometryConfig& geometry,
                 const app::SteeringConfig& steering_config,
                 const app::CalibrationConfig& calibration,
                 std::uint32_t sample_rate_hz,
                 std::size_t max_block_frames);
  void setTarget(audio::BeamformerSteering target);
  void setTuning(const MvdrTuningParams& tuning) noexcept;
  void setSpectralPostfilter(SpectralPostfilter* filter) noexcept { spectral_filter_ = filter; }
  void process(std::span<const audio::MicFrame> input, std::span<float> mono_out);
  void processStereo(std::span<const audio::MicFrame> input,
                     std::span<float> left_out,
                     std::span<float> right_out);
  [[nodiscard]] bool binauralOutputEnabled() const noexcept { return binaural_output_; }
  void resetStream() noexcept;
  [[nodiscard]] std::size_t algorithmicDelaySamples() const noexcept;
#ifdef SONITUDE_BEAMFORMER_TEST_HOOKS
  [[nodiscard]] std::uint64_t covarianceUpdateHopsForTest() const noexcept { return cov_update_hops_; }
  void resetCovarianceDiagnosticsForTest() noexcept { cov_update_hops_ = 0; }
  [[nodiscard]] bool crossfadingForTest() const noexcept { return crossfading_; }
  [[nodiscard]] std::size_t fadeCursorForTest() const noexcept { return fade_cursor_; }
  [[nodiscard]] audio::BeamformerSteering activeTargetForTest() const noexcept { return active_target_; }
  [[nodiscard]] audio::BeamformerSteering pendingTargetForTest() const noexcept { return pending_target_; }
#endif

 private:
  using DelayArray = std::array<double, audio::kMicChannels>;
  using MicPosArray = std::array<std::array<double, 3>, audio::kMicChannels>;

  struct MicHopContext
  {
    MvdrBeamformer* self = nullptr;
    std::size_t channel = 0;
  };

  DelayArray computeRelativeDelays(audio::BeamformerSteering target,
                                   std::size_t reference_mic_index) const;
  DelayArray computeBinauralDelays(audio::BeamformerSteering target, bool left_ear) const;
  [[nodiscard]] audio::BeamformerSteering normalizeSteeringTarget(
      audio::BeamformerSteering target) const noexcept;
  [[nodiscard]] bool steeringWithinDeadband(audio::BeamformerSteering a,
                                            audio::BeamformerSteering b) const noexcept;
  void assignPendingLook(audio::BeamformerSteering target) noexcept;
  void swapActivePendingPaths() noexcept;
  void pivotCrossfadeForRetarget() noexcept;
  void completeCrossfade() noexcept;
  void updateGuardDelays(const audio::BeamformerSteering& estimator_target) noexcept;
  static void OnMicHop(void* context, float* re, float* im, std::size_t fft_size) noexcept;
  void StoreMicSpectrum(std::size_t channel, const float* re, const float* im, std::size_t fft_size) noexcept;
  void FormLooksAndSynthesize(std::size_t fft_size) noexcept;
  void FormLookSpectrum(const DelayArray& delays, std::vector<float>& y_re, std::vector<float>& y_im) const noexcept;
  void ApplyHermitian(std::vector<float>& y_re, std::vector<float>& y_im, std::size_t fft_size) const noexcept;
  float PopMono();
  float PopEar(bool left_channel);

  bool configured_ = false;
  bool binaural_output_ = false;
  bool near_field_ = true;
  std::uint32_t sample_rate_hz_ = 0;
  std::size_t ramp_samples_ = 1;
  std::size_t fade_cursor_ = 0;
  bool crossfading_ = false;
  SpectralPostfilter* spectral_filter_ = nullptr;
  KemarSteeringLut steering_model_{};

  app::SteeringConfig steering_config_{};
  audio::BeamformerSteering active_target_{};
  audio::BeamformerSteering pending_target_{};
  MicPosArray mic_positions_{};
  DelayArray calibration_delays_{};
  DelayArray current_delays_{};
  DelayArray pending_delays_{};
  DelayArray current_left_delays_{};
  DelayArray pending_left_delays_{};
  DelayArray current_right_delays_{};
  DelayArray pending_right_delays_{};
  std::array<DelayArray, kGuardLooks> guard_delays_{};

  std::array<StreamingStft, audio::kMicChannels> mic_stft_{};
  std::array<MicHopContext, audio::kMicChannels> mic_ctx_{};
  StreamingStft target_stft_{};
  StreamingStft pending_stft_{};
  StreamingStft left_target_stft_{};
  StreamingStft left_pending_stft_{};
  StreamingStft right_target_stft_{};
  StreamingStft right_pending_stft_{};

  std::array<std::vector<float>, audio::kMicChannels> x_re_{};
  std::array<std::vector<float>, audio::kMicChannels> x_im_{};
  std::vector<float> y_re_{};
  std::vector<float> y_im_{};
  std::vector<float> pending_y_re_{};
  std::vector<float> pending_y_im_{};
  std::vector<float> left_y_re_{};
  std::vector<float> left_y_im_{};
  std::vector<float> right_y_re_{};
  std::vector<float> right_y_im_{};
  std::vector<float> pending_left_y_re_{};
  std::vector<float> pending_left_y_im_{};
  std::vector<float> pending_right_y_re_{};
  std::vector<float> pending_right_y_im_{};
  std::array<std::vector<float>, kGuardLooks> guard_y_re_{};
  std::array<std::vector<float>, kGuardLooks> guard_y_im_{};

  std::vector<std::array<std::array<std::array<float, 2>, audio::kMicChannels>, audio::kMicChannels>>
      cov_{};
  MvdrTuningParams tuning_{};
  float cov_beta_ = 0.02F;
#ifdef SONITUDE_BEAMFORMER_TEST_HOOKS
  std::uint64_t cov_update_hops_ = 0;
#endif
};
}  // namespace sonitude::dsp
