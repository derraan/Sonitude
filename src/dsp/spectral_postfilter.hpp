#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "dsp/streaming_stft.hpp"

namespace sonitude::dsp
{
// Running-minima noise PSD tracker. Baseline smoother-plus-minima rule, not
// Cohen IMCRA/MCRA: no bias correction, no two-stage search, no SPP as in
// those publications.
class MinimaNoiseTracker
{
 public:
  bool prepare(std::size_t n_bins, double hop_hz);
  void reset() noexcept;
  void update(std::span<const float> power, bool allow_update) noexcept;
  [[nodiscard]] std::span<const float> noisePower() const noexcept;
  [[nodiscard]] std::span<const float> smoothedPower() const noexcept;
  [[nodiscard]] std::size_t binCount() const noexcept { return n_bins_; }

 private:
  std::size_t n_bins_ = 0;
  float smooth_coeff_ = 0.5F;
  float rise_coeff_ = 0.01F;
  std::vector<float> smoothed_{};
  std::vector<float> noise_{};
  bool have_first_ = false;
};

// Bounded Wiener G = xi/(1+xi) with decision-directed a priori SNR, time
// smoothing, and a 3-bin frequency smoother. Gains clamped to [gain_floor, 1].
// Not Ephraim–Malah OM-LSA.
class BoundedWienerGain
{
 public:
  bool prepare(std::size_t n_bins, double hop_hz, float gain_floor_linear);
  void reset() noexcept;
  void compute(std::span<const float> power,
               std::span<const float> noise,
               std::span<float> gain_out) noexcept;
  [[nodiscard]] float meanGain() const noexcept { return mean_gain_; }
  [[nodiscard]] float gainFloor() const noexcept { return gain_floor_; }

 private:
  std::size_t n_bins_ = 0;
  float gain_floor_ = 0.25F;
  float dd_coeff_ = 0.9F;
  float time_coeff_ = 0.5F;
  float mean_gain_ = 1.0F;
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
};

class SpectralPostfilter
{
 public:
  bool prepare(double sample_rate,
               std::size_t maximum_block_frames,
               const SpectralPostfilterConfig& config);
  void reset() noexcept;
  void setControl(bool focus_active, float confidence) noexcept;
  void setEstimatorHold(bool hold) noexcept;
  void process(std::span<const float> input, std::span<float> output) noexcept;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] std::size_t algorithmicDelaySamples() const noexcept;
  [[nodiscard]] std::size_t fftSize() const noexcept { return stft_.fftSize(); }
  [[nodiscard]] std::size_t hopSize() const noexcept { return stft_.hopSize(); }
  [[nodiscard]] float gainFloorDb() const noexcept { return config_.gain_floor_db; }
  [[nodiscard]] float currentGain() const noexcept { return last_mean_gain_; }
  [[nodiscard]] std::size_t persistentBytes() const noexcept;
  [[nodiscard]] std::size_t calculatedLookaheadSamples() const noexcept
  {
    return stft_.calculatedLookaheadSamples();
  }

  MinimaNoiseTracker& noiseTracker() noexcept { return tracker_; }
  BoundedWienerGain& gainRule() noexcept { return wiener_; }

 private:
  static void OnHop(void* context, float* re, float* im, std::size_t fft_size) noexcept;
  void ProcessSpectrum(float* re, float* im, std::size_t fft_size) noexcept;

  bool ready_ = false;
  bool focus_active_ = true;
  float confidence_ = 1.0F;
  bool estimator_hold_ = false;
  std::size_t init_hops_remaining_ = 0;
  SpectralPostfilterConfig config_{};
  StreamingStft stft_{};
  MinimaNoiseTracker tracker_{};
  BoundedWienerGain wiener_{};
  std::vector<float> power_{};
  std::vector<float> gains_{};
  float last_mean_gain_ = 1.0F;
};
}  // namespace sonitude::dsp
