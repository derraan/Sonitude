#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "audio/audio_types.hpp"
#include "dsp/spatial_looks.hpp"

namespace sonitude::dsp
{
struct SpectralPostfilterConfig
{
  bool enabled = false;
  std::size_t fft_size = 128;
  std::size_t hop_size = 32;
  float gain_floor_db = -12.0F;
  float confidence_threshold = 0.6F;
  bool amplitude_range_bias = true;
  float speech_low_hz = 300.0F;
  float speech_high_hz = 4000.0F;
  float near_dominance_ratio = 1.4F;
};

struct SpectralTuningParams
{
  float gain_floor_db = -12.0F;
  float protect_ratio = 4.0F;
  float noise_overestimate = 2.0F;
  float tonal_median_ratio = 6.0F;
  float noise_rise_sec = 0.48F;
};

// Spectral gain stage for the MVDR 128/32 STFT. This class does not perform
// FFT, inverse FFT, or overlap-add. The beamformer owns the only transform.
class SpectralPostfilter
{
 public:
  bool prepare(double sample_rate,
               std::size_t maximum_block_frames,
               const SpectralPostfilterConfig& config);
  void reset() noexcept;
  void setControl(bool focus_active, float confidence) noexcept;
  void setConfidenceThreshold(float threshold) noexcept;
  void setTuning(const SpectralTuningParams& tuning) noexcept;
  void setEstimatorHold(bool hold) noexcept;
  void updateAmplitudeProximity(
      const std::array<std::span<const float>, audio::kMicChannels>& re,
      const std::array<std::span<const float>, audio::kMicChannels>& im) noexcept;
  void processSpectrum(std::span<float> re,
                       std::span<float> im,
                       GuardSpectrumConstSpans guards = {}) noexcept;
  // Apply the last hop's gains without updating the estimator. Used on the
  // outgoing look during a steering crossfade so one hop still has one update.
  void applyStoredGains(std::span<float> re, std::span<float> im) const noexcept;

  [[nodiscard]] float currentGain() const noexcept { return last_mean_gain_; }
  [[nodiscard]] float amplitudeProximityForTest() const noexcept { return proximity_; }

 private:
  class NoiseTracker
  {
   public:
    bool prepare(std::size_t n_bins, double hop_hz);
    void setNoiseRiseSec(float tau_sec, double hop_hz);
    void reset() noexcept;
    void update(std::span<const float> power, bool allow_update) noexcept;
    void update(std::span<const float> power,
                bool allow_update,
                std::span<const float> max_guard_power,
                float protect_ratio,
                float tonal_median_ratio) noexcept;
    [[nodiscard]] std::span<const float> noisePower() const noexcept;
    [[nodiscard]] bool initialized() const noexcept { return have_first_; }

   private:
    std::size_t n_bins_ = 0;
    float smooth_coeff_ = 0.5F;
    float rise_coeff_ = 0.01F;
    std::vector<float> smoothed_{};
    std::vector<float> noise_{};
    std::vector<float> median_scratch_{};
    bool have_first_ = false;
  };

  class WienerGain
  {
   public:
    bool prepare(std::size_t n_bins, double hop_hz, float gain_floor_linear);
    void setGainFloorLinear(float gain_floor_linear) noexcept;
    void reset() noexcept;
    void setNoiseOverestimate(float factor) noexcept;
    void compute(std::span<const float> power,
                 std::span<const float> noise,
                 std::span<float> gain_out) noexcept;
    [[nodiscard]] float gainFloor() const noexcept { return gain_floor_; }

   private:
    std::size_t n_bins_ = 0;
    double hop_hz_ = 0.0;
    float gain_floor_ = 0.25F;
    float noise_overestimate_ = 2.0F;
    float dd_coeff_ = 0.9F;
    float time_coeff_ = 0.5F;
    std::vector<float> xi_{};
    std::vector<float> gain_{};
    std::vector<float> prev_gain_{};
  };

  void RefreshSpeechBins() noexcept;

  bool ready_ = false;
  bool focus_active_ = true;
  bool have_spatial_ = false;
  float confidence_ = 1.0F;
  bool estimator_hold_ = false;
  double hop_hz_ = 0.0;
  double sample_rate_hz_ = 0.0;
  std::size_t speech_lo_bin_ = 1;
  std::size_t speech_hi_bin_ = 1;
  float proximity_ = 0.0F;
  SpectralPostfilterConfig config_{};
  SpectralTuningParams tuning_{};
  NoiseTracker tracker_{};
  WienerGain wiener_{};
  std::vector<float> power_{};
  std::vector<float> gains_{};
  std::vector<float> spatial_gain_{};
  std::vector<float> max_guard_power_{};
  float last_mean_gain_ = 1.0F;
  float apply_mix_ = 1.0F;
  float spatial_coeff_ = 0.5F;
};
}  // namespace sonitude::dsp
