#include "dsp/spectral_postfilter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace sonitude::dsp
{
namespace
{
constexpr float kEps = 1.0e-20F;
constexpr float kMaxXi = 1.0e6F;
constexpr float kNoiseOverestimate = 2.0F;
// Bins this far above the cross-frequency median of S are treated as tonal.
constexpr float kTonalMedianRatio = 6.0F;
constexpr float kTauPsdSmoothSec = 0.032F;
constexpr float kTauNoiseRiseSec = 0.48F;
constexpr float kTauDecisionDirectedSec = 0.048F;
constexpr float kTauGainSec = 0.016F;
constexpr float kTauBypassSec = 0.016F;

float CoeffFromTau(const float tau_sec, const double hop_hz)
{
  if (!(hop_hz > 0.0) || !(tau_sec > 0.0F))
  {
    return 0.0F;
  }
  const float hop_period = 1.0F / static_cast<float>(hop_hz);
  return std::exp(-hop_period / tau_sec);
}

float Sanitize(const float x)
{
  return std::isfinite(x) ? x : 0.0F;
}

float DbToLinear(const float db)
{
  return std::pow(10.0F, db / 20.0F);
}

float MedianOf(std::vector<float>& scratch, const std::size_t n)
{
  if (n == 0)
  {
    return kEps;
  }
  const std::size_t mid = n / 2U;
  std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid),
                   scratch.begin() + static_cast<std::ptrdiff_t>(n));
  return std::max(scratch[mid], kEps);
}
}  // namespace

bool AsymmetricNoisePowerTracker::prepare(const std::size_t n_bins, const double hop_hz)
{
  if (n_bins < 2 || !(hop_hz > 0.0))
  {
    return false;
  }
  n_bins_ = n_bins;
  smooth_coeff_ = CoeffFromTau(kTauPsdSmoothSec, hop_hz);
  rise_coeff_ = 1.0F - CoeffFromTau(kTauNoiseRiseSec, hop_hz);
  smoothed_.assign(n_bins, 0.0F);
  noise_.assign(n_bins, 1.0F);
  median_scratch_.assign(n_bins, 0.0F);
  have_first_ = false;
  return true;
}

void AsymmetricNoisePowerTracker::reset() noexcept
{
  std::fill(smoothed_.begin(), smoothed_.end(), 0.0F);
  std::fill(noise_.begin(), noise_.end(), 1.0F);
  have_first_ = false;
}

void AsymmetricNoisePowerTracker::update(const std::span<const float> power,
                                         const bool allow_update) noexcept
{
  const std::size_t n = std::min(n_bins_, power.size());
  if (!allow_update)
  {
    return;
  }
  if (!have_first_)
  {
    for (std::size_t k = 0; k < n; ++k)
    {
      const float p = std::max(Sanitize(power[k]), kEps);
      smoothed_[k] = p;
      median_scratch_[k] = p;
    }
    const float med = MedianOf(median_scratch_, n);
    for (std::size_t k = 0; k < n; ++k)
    {
      noise_[k] = med;
    }
    have_first_ = true;
    return;
  }
  for (std::size_t k = 0; k < n; ++k)
  {
    const float p = std::max(Sanitize(power[k]), kEps);
    smoothed_[k] = (smooth_coeff_ * smoothed_[k]) + ((1.0F - smooth_coeff_) * p);
    median_scratch_[k] = smoothed_[k];
  }
  const float med = MedianOf(median_scratch_, n);
  const float tonal_floor = med * kTonalMedianRatio;
  for (std::size_t k = 0; k < n; ++k)
  {
    if (smoothed_[k] > tonal_floor)
    {
      continue;
    }
    if (smoothed_[k] < noise_[k])
    {
      noise_[k] = std::max(smoothed_[k], kEps);
    }
    else
    {
      noise_[k] += rise_coeff_ * (smoothed_[k] - noise_[k]);
      noise_[k] = std::max(noise_[k], kEps);
    }
  }
}

std::span<const float> AsymmetricNoisePowerTracker::noisePower() const noexcept
{
  return std::span<const float>(noise_.data(), n_bins_);
}

std::span<const float> AsymmetricNoisePowerTracker::smoothedPower() const noexcept
{
  return std::span<const float>(smoothed_.data(), n_bins_);
}

std::size_t AsymmetricNoisePowerTracker::persistentBytes() const noexcept
{
  return (smoothed_.size() + noise_.size() + median_scratch_.size()) * sizeof(float);
}

bool BoundedWienerGain::prepare(const std::size_t n_bins,
                                const double hop_hz,
                                const float gain_floor_linear)
{
  if (n_bins < 2 || !(hop_hz > 0.0) || !(gain_floor_linear >= 0.0F) || gain_floor_linear > 1.0F)
  {
    return false;
  }
  n_bins_ = n_bins;
  gain_floor_ = gain_floor_linear;
  dd_coeff_ = CoeffFromTau(kTauDecisionDirectedSec, hop_hz);
  time_coeff_ = CoeffFromTau(kTauGainSec, hop_hz);
  xi_.assign(n_bins, 0.0F);
  gain_.assign(n_bins, gain_floor_);
  prev_gain_.assign(n_bins, gain_floor_);
  mean_gain_ = gain_floor_;
  return true;
}

void BoundedWienerGain::reset() noexcept
{
  std::fill(xi_.begin(), xi_.end(), 0.0F);
  std::fill(gain_.begin(), gain_.end(), gain_floor_);
  std::fill(prev_gain_.begin(), prev_gain_.end(), gain_floor_);
  mean_gain_ = gain_floor_;
}

void BoundedWienerGain::compute(const std::span<const float> power,
                                const std::span<const float> noise,
                                const std::span<float> gain_out) noexcept
{
  const std::size_t n = std::min(n_bins_, std::min(power.size(), std::min(noise.size(), gain_out.size())));
  for (std::size_t k = 0; k < n; ++k)
  {
    const float p = std::max(Sanitize(power[k]), kEps);
    const float lambda = std::max(Sanitize(noise[k]) * kNoiseOverestimate, kEps);
    const float snr_post = p / lambda;
    const float instant = std::max(snr_post - 1.0F, 0.0F);
    const float dd = prev_gain_[k] * prev_gain_[k] * snr_post;
    float xi = (dd_coeff_ * dd) + ((1.0F - dd_coeff_) * instant);
    xi = std::clamp(xi, 0.0F, kMaxXi);
    xi_[k] = xi;
    float g = xi / (1.0F + xi);
    g = std::clamp(g, gain_floor_, 1.0F);
    g = (time_coeff_ * prev_gain_[k]) + ((1.0F - time_coeff_) * g);
    g = std::clamp(g, gain_floor_, 1.0F);
    gain_[k] = g;
  }
  if (n > 0)
  {
    gain_out[0] = gain_[0];
  }
  if (n > 1)
  {
    gain_out[n - 1] = gain_[n - 1];
  }
  for (std::size_t k = 1; k + 1 < n; ++k)
  {
    gain_out[k] = (0.25F * gain_[k - 1]) + (0.5F * gain_[k]) + (0.25F * gain_[k + 1]);
    gain_out[k] = std::clamp(gain_out[k], gain_floor_, 1.0F);
  }
  double sum = 0.0;
  for (std::size_t k = 0; k < n; ++k)
  {
    prev_gain_[k] = gain_out[k];
    sum += gain_out[k];
  }
  mean_gain_ = n > 0 ? static_cast<float>(sum / static_cast<double>(n)) : gain_floor_;
}

std::size_t BoundedWienerGain::persistentBytes() const noexcept
{
  return (xi_.size() + gain_.size() + prev_gain_.size()) * sizeof(float);
}

bool SpectralPostfilter::prepare(const double sample_rate,
                                 const std::size_t maximum_block_frames,
                                 const SpectralPostfilterConfig& config)
{
  ready_ = false;
  if (!config.enabled)
  {
    return false;
  }
  if (config.gain_floor_db > 0.0F || config.gain_floor_db < -80.0F)
  {
    return false;
  }
  if (!stft_.prepare(sample_rate, maximum_block_frames, {config.fft_size, config.hop_size}))
  {
    return false;
  }
  const std::size_t n_bins = (config.fft_size / 2U) + 1U;
  const double hop_hz = sample_rate / static_cast<double>(config.hop_size);
  const float floor_lin = std::clamp(DbToLinear(config.gain_floor_db), 0.0F, 1.0F);
  if (!tracker_.prepare(n_bins, hop_hz) || !wiener_.prepare(n_bins, hop_hz, floor_lin))
  {
    return false;
  }
  config_ = config;
  config_.confidence_threshold = std::clamp(config.confidence_threshold, 0.0F, 1.0F);
  power_.assign(n_bins, 0.0F);
  gains_.assign(n_bins, 1.0F);
  last_mean_gain_ = 1.0F;
  apply_mix_ = 0.0F;
  focus_active_ = true;
  confidence_ = 1.0F;
  estimator_hold_ = false;
  ready_ = true;
  return true;
}

void SpectralPostfilter::reset() noexcept
{
  stft_.reset();
  tracker_.reset();
  wiener_.reset();
  std::fill(power_.begin(), power_.end(), 0.0F);
  std::fill(gains_.begin(), gains_.end(), 1.0F);
  last_mean_gain_ = 1.0F;
  apply_mix_ = 0.0F;
  estimator_hold_ = false;
}

void SpectralPostfilter::setControl(const bool focus_active, const float confidence) noexcept
{
  focus_active_ = focus_active;
  confidence_ = std::clamp(confidence, 0.0F, 1.0F);
}

void SpectralPostfilter::setConfidenceThreshold(const float threshold) noexcept
{
  config_.confidence_threshold = std::clamp(threshold, 0.0F, 1.0F);
}

void SpectralPostfilter::setEstimatorHold(const bool hold) noexcept
{
  estimator_hold_ = hold;
}

void SpectralPostfilter::OnHop(void* context, float* re, float* im, const std::size_t fft_size) noexcept
{
  static_cast<SpectralPostfilter*>(context)->ProcessSpectrum(re, im, fft_size);
}

void SpectralPostfilter::ProcessSpectrum(float* const re, float* const im, const std::size_t fft_size) noexcept
{
  const std::size_t n_bins = (fft_size / 2U) + 1U;
  for (std::size_t k = 0; k < n_bins; ++k)
  {
    const float r = Sanitize(re[k]);
    const float i = Sanitize(im[k]);
    power_[k] = (r * r) + (i * i);
  }

  const bool focused = focus_active_ && (confidence_ >= config_.confidence_threshold);
  const bool allow_noise = focused && !estimator_hold_;
  tracker_.update(power_, allow_noise);

  const double hop_hz =
      stft_.hopSize() == 0 ? 0.0 : stft_.sampleRate() / static_cast<double>(stft_.hopSize());
  const float mix_coeff = CoeffFromTau(kTauBypassSec, hop_hz);
  const float target_mix = (focused && tracker_.initialized()) ? 1.0F : 0.0F;
  apply_mix_ = (mix_coeff * apply_mix_) + ((1.0F - mix_coeff) * target_mix);
  apply_mix_ = std::clamp(apply_mix_, 0.0F, 1.0F);

  if (tracker_.initialized() && apply_mix_ > 1.0e-4F)
  {
    wiener_.compute(power_, tracker_.noisePower(), gains_);
  }
  else
  {
    std::fill(gains_.begin(), gains_.end(), 1.0F);
  }

  double weighted = 0.0;
  double power_sum = 0.0;
  for (std::size_t k = 0; k < n_bins; ++k)
  {
    float g = std::clamp(Sanitize(gains_[k]), 0.0F, 1.0F);
    g = ((1.0F - apply_mix_) * 1.0F) + (apply_mix_ * g);
    g = std::clamp(g, 0.0F, 1.0F);
    gains_[k] = g;
    re[k] *= g;
    im[k] *= g;
    if (k != 0 && k * 2U != fft_size)
    {
      const std::size_t ck = fft_size - k;
      re[ck] *= g;
      im[ck] *= g;
    }
    weighted += static_cast<double>(g) * static_cast<double>(power_[k]);
    power_sum += static_cast<double>(power_[k]);
  }
  last_mean_gain_ =
      power_sum > static_cast<double>(kEps) ? static_cast<float>(weighted / power_sum) : 1.0F;
}

void SpectralPostfilter::process(const std::span<const float> input, const std::span<float> output) noexcept
{
  if (!ready_)
  {
    const std::size_t n = std::min(input.size(), output.size());
    for (std::size_t i = 0; i < n; ++i)
    {
      output[i] = Sanitize(input[i]);
    }
    for (std::size_t i = n; i < output.size(); ++i)
    {
      output[i] = 0.0F;
    }
    return;
  }
  if (input.data() == output.data())
  {
    for (float& s : output)
    {
      s = Sanitize(s);
    }
    stft_.process(output, output, &SpectralPostfilter::OnHop, this);
    return;
  }
  for (std::size_t i = 0; i < input.size() && i < output.size(); ++i)
  {
    output[i] = Sanitize(input[i]);
  }
  stft_.process(std::span<const float>(output.data(), std::min(input.size(), output.size())),
                output,
                &SpectralPostfilter::OnHop,
                this);
}

std::size_t SpectralPostfilter::algorithmicDelaySamples() const noexcept
{
  const std::size_t n = stft_.fftSize();
  return n == 0 ? 0 : n - 1U;
}

std::size_t SpectralPostfilter::persistentBytes() const noexcept
{
  return stft_.persistentBytes() + tracker_.persistentBytes() + wiener_.persistentBytes() +
         ((power_.size() + gains_.size()) * sizeof(float));
}
}  // namespace sonitude::dsp
