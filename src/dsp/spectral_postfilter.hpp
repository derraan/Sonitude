#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "dsp/spatial_looks.hpp"

namespace sonitude::dsp
{
class AsymmetricNoisePowerTracker
{
 public:
  bool prepare(std::size_t n_bins, double hop_hz);
  void reset() noexcept;
  void update(std::span<const float> power, bool allow_update) noexcept;
  void update(std::span<const float> power,
              bool allow_update,
              std::span<const float> max_guard_power,
              float protect_ratio) noexcept;
  [[nodiscard]] std::span<const float> noisePower() const noexcept;
  [[nodiscard]] bool initialized() const noexcept { return have_first_; }
  [[nodiscard]] std::size_t persistentBytes() const noexcept;

 private:
  std::size_t n_bins_ = 0;
  float smooth_coeff_ = 0.5F;
  float rise_coeff_ = 0.01F;
  std::vector<float> smoothed_{};
  std::vector<float> noise_{};
  std::vector<float> median_scratch_{};
  bool have_first_ = false;
};

class BoundedWienerGain
{
 public:
  bool prepare(std::size_t n_bins, double hop_hz, float gain_floor_linear);
  void reset() noexcept;
  void compute(std::span<const float> power,
               std::span<const float> noise,
               std::span<float> gain_out) noexcept;
  [[nodiscard]] float gainFloor() const noexcept { return gain_floor_; }
  [[nodiscard]] std::size_t persistentBytes() const noexcept;

 private:
  std::size_t n_bins_ = 0;
  float gain_floor_ = 0.25F;
  float dd_coeff_ = 0.9F;
  float time_coeff_ = 0.5F;
  std::vector<float> xi_{};
  std::vector<float> gain_{};
  std::vector<float> prev_gain_{};
};

struct SpectralPostfilterConfig
{
  bool enabled = false;
  std::size_t fft_size = 128;
  std::size_t hop_size = 32;
  float gain_floor_db = -12.0F;
  float confidence_threshold = 0.6F;
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
  void setEstimatorHold(bool hold) noexcept;
  void processSpectrum(std::span<float> re,
                       std::span<float> im,
                       GuardSpectrumConstSpans guards = {}) noexcept;

  [[nodiscard]] std::size_t algorithmicDelaySamples() const noexcept { return 0; }
  [[nodiscard]] float currentGain() const noexcept { return last_mean_gain_; }
  [[nodiscard]] std::size_t persistentBytes() const noexcept;

 private:
  bool ready_ = false;
  bool focus_active_ = true;
  bool have_spatial_ = false;
  float confidence_ = 1.0F;
  bool estimator_hold_ = false;
  double hop_hz_ = 0.0;
  SpectralPostfilterConfig config_{};
  AsymmetricNoisePowerTracker tracker_{};
  BoundedWienerGain wiener_{};
  std::vector<float> power_{};
  std::vector<float> gains_{};
  std::vector<float> spatial_gain_{};
  std::vector<float> max_guard_power_{};
  float last_mean_gain_ = 1.0F;
  float apply_mix_ = 1.0F;
  float spatial_coeff_ = 0.5F;
};
}  // namespace sonitude::dsp
