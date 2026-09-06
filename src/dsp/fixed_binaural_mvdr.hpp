#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "audio/audio_types.hpp"
#include "dsp/array_profile.hpp"
#include "dsp/streaming_stft.hpp"

namespace sonitude::dsp
{
struct FixedMvdrMaskParams
{
  bool enabled = true;
  float eta_low_db = -3.0F;
  float eta_high_db = 3.0F;
  float smooth_sec = 0.020F;
  float quiet_power = 1.0e-8F;
  float constant_mask = -1.0F;  // <0 uses residual estimator
};

class FixedBinauralMvdr
{
 public:
  void configure(const ArrayProfile& profile,
                 std::uint32_t sample_rate_hz,
                 std::size_t max_block_frames,
                 const FixedMvdrMaskParams& mask = {});
  void setTarget(audio::BeamformerSteering target);
  void processStereo(std::span<const audio::MicFrame> input,
                     std::span<float> left_out,
                     std::span<float> right_out);
  void resetStream() noexcept;
  [[nodiscard]] std::size_t algorithmicDelaySamples() const noexcept;
  [[nodiscard]] std::uint64_t factorizationCountForTest() const noexcept { return 0; }
  [[nodiscard]] std::uint64_t solveCountForTest() const noexcept { return 0; }
  [[nodiscard]] std::uint64_t lookCountForTest() const noexcept { return look_count_; }
  [[nodiscard]] bool crossfadingForTest() const noexcept { return crossfading_; }

 private:
  struct MicHopContext
  {
    FixedBinauralMvdr* self = nullptr;
    std::size_t channel = 0;
  };

  static void OnMicHop(void* context, float* re, float* im, std::size_t fft_size) noexcept;
  void StoreMicSpectrum(std::size_t channel, const float* re, const float* im, std::size_t fft_size) noexcept;
  void FormAndSynthesize() noexcept;
  void ApplyHermitian(std::vector<float>& y_re, std::vector<float>& y_im) const noexcept;
  void FormEarSpectra(std::size_t dir_index,
                      std::vector<float>& left_re,
                      std::vector<float>& left_im,
                      std::vector<float>& right_re,
                      std::vector<float>& right_im,
                      bool update_smoother) noexcept;

  bool configured_ = false;
  ArrayProfile profile_{};
  FixedMvdrMaskParams mask_{};
  std::uint32_t sample_rate_hz_ = 0;
  std::size_t ramp_samples_ = 1;
  std::size_t fade_cursor_ = 0;
  bool crossfading_ = false;
  float active_azimuth_ = 0.0F;
  float pending_azimuth_ = 0.0F;
  float queued_azimuth_ = 0.0F;
  bool have_queued_ = false;
  std::size_t active_dir_ = 0;
  std::size_t pending_dir_ = 0;

  std::array<StreamingStft, audio::kMicChannels> mic_stft_{};
  std::array<MicHopContext, audio::kMicChannels> mic_ctx_{};
  StreamingStft left_stft_{};
  StreamingStft right_stft_{};

  std::array<std::vector<float>, audio::kMicChannels> x_re_{};
  std::array<std::vector<float>, audio::kMicChannels> x_im_{};
  std::vector<float> left_y_re_{};
  std::vector<float> left_y_im_{};
  std::vector<float> right_y_re_{};
  std::vector<float> right_y_im_{};
  std::vector<float> pending_left_re_{};
  std::vector<float> pending_left_im_{};
  std::vector<float> pending_right_re_{};
  std::vector<float> pending_right_im_{};
  std::vector<float> pz_smooth_{};
  std::vector<float> pr_smooth_{};
  float mask_alpha_ = 1.0F;
  std::uint64_t look_count_ = 0;
};
}  // namespace sonitude::dsp
