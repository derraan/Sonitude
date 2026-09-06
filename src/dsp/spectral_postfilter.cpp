#include "dsp/spectral_postfilter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sonitude::dsp
{
namespace
{
constexpr float kEps = 1.0e-20F;
constexpr float kMaxXi = 1.0e6F;
constexpr float kSpatialBias = 1.0F;
constexpr float kTauPsdSmoothSec = 0.032F;
constexpr float kTauDecisionDirectedSec = 0.048F;
constexpr float kTauGainSec = 0.016F;
constexpr float kTauBypassSec = 0.016F;
constexpr float kTauSpatialSec = 0.016F;
constexpr std::size_t kSharedFftSize = 128;
constexpr std::size_t kSharedHopSize = 32;

float CoeffFromTau(const float tau_sec, const double hop_hz)
{
  if (!(hop_hz > 0.0) || !(tau_sec > 0.0F))
  {
    return 0.0F;
  }
  return std::exp(-(1.0F / static_cast<float>(hop_hz)) / tau_sec);
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

bool SpectralPostfilter::NoiseTracker::prepare(const std::size_t n_bins, const double hop_hz)
{
  if (n_bins < 2 || !(hop_hz > 0.0))
  {
    return false;
  }
  n_bins_ = n_bins;
  smooth_coeff_ = CoeffFromTau(kTauPsdSmoothSec, hop_hz);
  smoothed_.assign(n_bins, 0.0F);
  noise_.assign(n_bins, 1.0F);
  median_scratch_.assign(n_bins, 0.0F);
  have_first_ = false;
  return true;
}

void SpectralPostfilter::NoiseTracker::setNoiseRiseSec(const float tau_sec, const double hop_hz)
{
  rise_coeff_ = 1.0F - CoeffFromTau(std::max(tau_sec, 1.0e-3F), hop_hz);
}

void SpectralPostfilter::NoiseTracker::reset() noexcept
{
  std::fill(smoothed_.begin(), smoothed_.end(), 0.0F);
  std::fill(noise_.begin(), noise_.end(), 1.0F);
  have_first_ = false;
}

void SpectralPostfilter::NoiseTracker::update(const std::span<const float> power,
                                         const bool allow_update) noexcept
{
  update(power, allow_update, {}, 0.0F, 6.0F);
}

void SpectralPostfilter::NoiseTracker::update(const std::span<const float> power,
                                         const bool allow_update,
                                         const std::span<const float> max_guard_power,
                                         const float protect_ratio,
                                         const float tonal_median_ratio) noexcept
{
  const std::size_t n = std::min(n_bins_, power.size());
  if (!allow_update)
  {
    return;
  }
  const bool spatial = max_guard_power.size() >= n && protect_ratio > 0.0F;
  auto protected_bin = [&](const std::size_t k) {
    if (!spatial)
    {
      return false;
    }
    const float pg = std::max(Sanitize(max_guard_power[k]), kEps);
    return (std::max(Sanitize(power[k]), kEps) / pg) > protect_ratio;
  };

  if (!have_first_)
  {
    std::size_t used = 0;
    for (std::size_t k = 0; k < n; ++k)
    {
      const float p = std::max(Sanitize(power[k]), kEps);
      smoothed_[k] = p;
      if (!protected_bin(k))
      {
        median_scratch_[used++] = p;
      }
    }
    if (used == 0)
    {
      return;
    }
    const float med = MedianOf(median_scratch_, used);
    std::fill(noise_.begin(), noise_.begin() + static_cast<std::ptrdiff_t>(n), med);
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
  const float tonal_floor = med * std::max(tonal_median_ratio, 1.0F);
  for (std::size_t k = 0; k < n; ++k)
  {
    if (protected_bin(k) || smoothed_[k] > tonal_floor)
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

std::span<const float> SpectralPostfilter::NoiseTracker::noisePower() const noexcept
{
  return std::span<const float>(noise_.data(), n_bins_);
}

bool SpectralPostfilter::WienerGain::prepare(const std::size_t n_bins,
                                const double hop_hz,
                                const float gain_floor_linear)
{
  if (n_bins < 2 || !(hop_hz > 0.0) || gain_floor_linear < 0.0F || gain_floor_linear > 1.0F)
  {
    return false;
  }
  const bool structural = n_bins_ != n_bins || n_bins_ == 0;
  n_bins_ = n_bins;
  hop_hz_ = hop_hz;
  dd_coeff_ = CoeffFromTau(kTauDecisionDirectedSec, hop_hz);
  time_coeff_ = CoeffFromTau(kTauGainSec, hop_hz);
  if (structural)
  {
    gain_floor_ = gain_floor_linear;
    xi_.assign(n_bins, 0.0F);
    gain_.assign(n_bins, gain_floor_);
    prev_gain_.assign(n_bins, gain_floor_);
  }
  else
  {
    setGainFloorLinear(gain_floor_linear);
  }
  return true;
}

void SpectralPostfilter::WienerGain::setGainFloorLinear(const float gain_floor_linear) noexcept
{
  gain_floor_ = std::clamp(gain_floor_linear, 0.0F, 1.0F);
  for (float& g : gain_)
  {
    g = std::max(g, gain_floor_);
  }
  for (float& pg : prev_gain_)
  {
    pg = std::max(pg, gain_floor_);
  }
}

void SpectralPostfilter::WienerGain::reset() noexcept
{
  std::fill(xi_.begin(), xi_.end(), 0.0F);
  std::fill(gain_.begin(), gain_.end(), gain_floor_);
  std::fill(prev_gain_.begin(), prev_gain_.end(), gain_floor_);
}

void SpectralPostfilter::WienerGain::setNoiseOverestimate(const float factor) noexcept
{
  noise_overestimate_ = std::max(factor, 1.0F);
}

void SpectralPostfilter::WienerGain::compute(const std::span<const float> power,
                                const std::span<const float> noise,
                                const std::span<float> gain_out) noexcept
{
  const std::size_t n = std::min(n_bins_, std::min(power.size(), std::min(noise.size(), gain_out.size())));
  for (std::size_t k = 0; k < n; ++k)
  {
    const float p = std::max(Sanitize(power[k]), kEps);
    const float lambda = std::max(Sanitize(noise[k]) * noise_overestimate_, kEps);
    const float snr_post = p / lambda;
    const float instant = std::max(snr_post - 1.0F, 0.0F);
    const float dd = prev_gain_[k] * prev_gain_[k] * snr_post;
    const float xi = std::clamp((dd_coeff_ * dd) + ((1.0F - dd_coeff_) * instant), 0.0F, kMaxXi);
    xi_[k] = xi;
    float gain = std::clamp(xi / (1.0F + xi), gain_floor_, 1.0F);
    gain = std::clamp((time_coeff_ * prev_gain_[k]) + ((1.0F - time_coeff_) * gain),
                      gain_floor_, 1.0F);
    gain_[k] = gain;
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
    gain_out[k] = std::clamp((0.25F * gain_[k - 1]) + (0.5F * gain_[k]) +
                                 (0.25F * gain_[k + 1]),
                             gain_floor_, 1.0F);
  }
  for (std::size_t k = 0; k < n; ++k)
  {
    prev_gain_[k] = gain_out[k];
  }
}

bool SpectralPostfilter::prepare(const double sample_rate,
                                 const std::size_t maximum_block_frames,
                                 const SpectralPostfilterConfig& config)
{
  (void)maximum_block_frames;
  ready_ = false;
  if (!config.enabled || !(sample_rate > 0.0) || config.fft_size != kSharedFftSize ||
      config.hop_size != kSharedHopSize || config.gain_floor_db > 0.0F ||
      config.gain_floor_db < -80.0F)
  {
    return false;
  }

  config_ = config;
  config_.confidence_threshold = std::clamp(config.confidence_threshold, 0.0F, 1.0F);
  tuning_.gain_floor_db = std::clamp(config_.gain_floor_db, -80.0F, 0.0F);
  hop_hz_ = sample_rate / static_cast<double>(config_.hop_size);
  const std::size_t n_bins = (config_.fft_size / 2U) + 1U;
  const float floor_lin = std::clamp(DbToLinear(tuning_.gain_floor_db), 0.0F, 1.0F);
  if (!tracker_.prepare(n_bins, hop_hz_) || !wiener_.prepare(n_bins, hop_hz_, floor_lin))
  {
    return false;
  }
  tracker_.setNoiseRiseSec(tuning_.noise_rise_sec, hop_hz_);
  wiener_.setNoiseOverestimate(tuning_.noise_overestimate);

  power_.assign(n_bins, 0.0F);
  gains_.assign(n_bins, 1.0F);
  spatial_gain_.assign(n_bins, 1.0F);
  max_guard_power_.assign(n_bins, 0.0F);
  spatial_coeff_ = CoeffFromTau(kTauSpatialSec, hop_hz_);
  last_mean_gain_ = 1.0F;
  apply_mix_ = 0.0F;
  have_spatial_ = false;
  focus_active_ = true;
  confidence_ = 1.0F;
  estimator_hold_ = false;
  ready_ = true;
  return true;
}

void SpectralPostfilter::reset() noexcept
{
  tracker_.reset();
  wiener_.reset();
  std::fill(power_.begin(), power_.end(), 0.0F);
  std::fill(gains_.begin(), gains_.end(), 1.0F);
  std::fill(spatial_gain_.begin(), spatial_gain_.end(), 1.0F);
  std::fill(max_guard_power_.begin(), max_guard_power_.end(), 0.0F);
  last_mean_gain_ = 1.0F;
  apply_mix_ = 0.0F;
  have_spatial_ = false;
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

void SpectralPostfilter::setTuning(const SpectralTuningParams& tuning) noexcept
{
  const SpectralTuningParams next{
      .gain_floor_db = std::clamp(tuning.gain_floor_db, -80.0F, 0.0F),
      .protect_ratio = std::clamp(tuning.protect_ratio, 1.0F, 16.0F),
      .noise_overestimate = std::clamp(tuning.noise_overestimate, 1.0F, 8.0F),
      .tonal_median_ratio = std::clamp(tuning.tonal_median_ratio, 2.0F, 16.0F),
      .noise_rise_sec = std::clamp(tuning.noise_rise_sec, 0.05F, 4.0F)};
  if (next.gain_floor_db == tuning_.gain_floor_db && next.protect_ratio == tuning_.protect_ratio &&
      next.noise_overestimate == tuning_.noise_overestimate &&
      next.tonal_median_ratio == tuning_.tonal_median_ratio &&
      next.noise_rise_sec == tuning_.noise_rise_sec)
  {
    return;
  }
  const float prev_floor_db = tuning_.gain_floor_db;
  tuning_ = next;
  if (!ready_)
  {
    return;
  }
  if (tuning_.gain_floor_db != prev_floor_db)
  {
    wiener_.setGainFloorLinear(std::clamp(DbToLinear(tuning_.gain_floor_db), 0.0F, 1.0F));
  }
  tracker_.setNoiseRiseSec(tuning_.noise_rise_sec, hop_hz_);
  wiener_.setNoiseOverestimate(tuning_.noise_overestimate);
}

void SpectralPostfilter::setEstimatorHold(const bool hold) noexcept
{
  estimator_hold_ = hold;
}

void SpectralPostfilter::processSpectrum(const std::span<float> re,
                                         const std::span<float> im,
                                         const GuardSpectrumConstSpans guards) noexcept
{
  if (!ready_ || re.size() < config_.fft_size || im.size() < config_.fft_size)
  {
    return;
  }

  const std::size_t n_bins = power_.size();
  have_spatial_ = true;
  for (std::size_t g = 0; g < kGuardLooks; ++g)
  {
    have_spatial_ = have_spatial_ && guards.re[g].size() >= n_bins && guards.im[g].size() >= n_bins;
  }

  for (std::size_t k = 0; k < n_bins; ++k)
  {
    const float r = Sanitize(re[k]);
    const float i = Sanitize(im[k]);
    power_[k] = (r * r) + (i * i);
    float guard_power = kEps;
    if (have_spatial_)
    {
      for (std::size_t g = 0; g < kGuardLooks; ++g)
      {
        const float gr = Sanitize(guards.re[g][k]);
        const float gi = Sanitize(guards.im[g][k]);
        guard_power = std::max(guard_power, (gr * gr) + (gi * gi));
      }
    }
    max_guard_power_[k] = guard_power;
  }

  const bool focused = focus_active_ && confidence_ >= config_.confidence_threshold;
  const bool allow_noise = focused && !estimator_hold_;
  if (have_spatial_)
  {
    tracker_.update(power_, allow_noise, max_guard_power_, tuning_.protect_ratio,
                    tuning_.tonal_median_ratio);
  }
  else
  {
    tracker_.update(power_, allow_noise);
  }

  const float mix_coeff = CoeffFromTau(kTauBypassSec, hop_hz_);
  const float target_mix = focused && tracker_.initialized() ? 1.0F : 0.0F;
  apply_mix_ = std::clamp((mix_coeff * apply_mix_) + ((1.0F - mix_coeff) * target_mix), 0.0F, 1.0F);

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
    float spatial_gain = 1.0F;
    bool protected_bin = false;
    if (have_spatial_)
    {
      const float ratio = power_[k] / std::max(max_guard_power_[k], kEps);
      protected_bin = ratio > tuning_.protect_ratio;
      const float instant = ratio / (ratio + kSpatialBias);
      spatial_gain_[k] = (spatial_coeff_ * spatial_gain_[k]) +
                         ((1.0F - spatial_coeff_) * instant);
      spatial_gain = std::clamp(spatial_gain_[k], 0.0F, 1.0F);
    }

    const float wiener_gain = std::clamp(Sanitize(gains_[k]), 0.0F, 1.0F);
    float gain = protected_bin ? std::max(spatial_gain, wiener_gain)
                               : (spatial_gain * wiener_gain);
    gain = ((1.0F - apply_mix_) * 1.0F) + (apply_mix_ * gain);
    if (have_spatial_ && focused && apply_mix_ < 1.0e-4F)
    {
      gain = spatial_gain;
    }
    gain = std::clamp(gain, wiener_.gainFloor(), 1.0F);
    gains_[k] = gain;

    re[k] = Sanitize(re[k]) * gain;
    im[k] = Sanitize(im[k]) * gain;
    if (k != 0U && k * 2U != config_.fft_size)
    {
      const std::size_t mirror = config_.fft_size - k;
      re[mirror] = Sanitize(re[mirror]) * gain;
      im[mirror] = Sanitize(im[mirror]) * gain;
    }
    weighted += static_cast<double>(gain) * static_cast<double>(power_[k]);
    power_sum += static_cast<double>(power_[k]);
  }
  last_mean_gain_ = power_sum > static_cast<double>(kEps)
                        ? static_cast<float>(weighted / power_sum)
                        : 1.0F;
}

void SpectralPostfilter::applyStoredGains(const std::span<float> re,
                                          const std::span<float> im) const noexcept
{
  if (!ready_ || re.size() < config_.fft_size || im.size() < config_.fft_size)
  {
    return;
  }
  const std::size_t n_bins = gains_.size();
  for (std::size_t k = 0; k < n_bins; ++k)
  {
    const float gain = std::clamp(Sanitize(gains_[k]), 0.0F, 1.0F);
    re[k] = Sanitize(re[k]) * gain;
    im[k] = Sanitize(im[k]) * gain;
    if (k != 0U && k * 2U != config_.fft_size)
    {
      const std::size_t mirror = config_.fft_size - k;
      re[mirror] = Sanitize(re[mirror]) * gain;
      im[mirror] = Sanitize(im[mirror]) * gain;
    }
  }
}

}  // namespace sonitude::dsp
