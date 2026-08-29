#include "dsp/spectral_postfilter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sonitude::dsp
{
namespace
{
constexpr float kEps = 1.0e-20F;
constexpr float kInitSeconds = 0.050F;
constexpr float kMaxXi = 1.0e6F;
constexpr float kNoiseOverestimate = 2.0F;
// Presence: freeze noise updates when a-posteriori SNR exceeds ~6 dB.
constexpr float kPresenceSnr = 10.0F;
constexpr std::size_t kPresenceBinHits = 3;
// Time constants in seconds, converted with hop rate in prepare().
constexpr float kTauPsdSmoothSec = 0.032F;
constexpr float kTauNoiseRiseSec = 0.48F;
constexpr float kTauDecisionDirectedSec = 0.048F;
constexpr float kTauGainSec = 0.016F;
constexpr float kConfidenceThreshold = 0.6F;

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
}  // namespace

bool MinimaNoiseTracker::prepare(const std::size_t n_bins, const double hop_hz)
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
  have_first_ = false;
  return true;
}

void MinimaNoiseTracker::reset() noexcept
{
  std::fill(smoothed_.begin(), smoothed_.end(), 0.0F);
  std::fill(noise_.begin(), noise_.end(), 1.0F);
  have_first_ = false;
}

void MinimaNoiseTracker::update(const std::span<const float> power, const bool allow_update) noexcept
{
  const std::size_t n = std::min(n_bins_, power.size());
  if (!have_first_)
  {
    for (std::size_t k = 0; k < n; ++k)
    {
      const float p = std::max(Sanitize(power[k]), kEps);
      smoothed_[k] = p;
      noise_[k] = p;
    }
    have_first_ = true;
    return;
  }
  for (std::size_t k = 0; k < n; ++k)
  {
    const float p = std::max(Sanitize(power[k]), kEps);
    smoothed_[k] = (smooth_coeff_ * smoothed_[k]) + ((1.0F - smooth_coeff_) * p);
    if (smoothed_[k] < noise_[k])
    {
      noise_[k] = std::max(smoothed_[k], kEps);
    }
    else if (allow_update)
    {
      noise_[k] += rise_coeff_ * (smoothed_[k] - noise_[k]);
      noise_[k] = std::max(noise_[k], kEps);
    }
  }
}

std::span<const float> MinimaNoiseTracker::noisePower() const noexcept
{
  return std::span<const float>(noise_.data(), n_bins_);
}

std::span<const float> MinimaNoiseTracker::smoothedPower() const noexcept
{
  return std::span<const float>(smoothed_.data(), n_bins_);
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
  config_.gain_floor_db = config.gain_floor_db;
  power_.assign(n_bins, 0.0F);
  gains_.assign(n_bins, floor_lin);
  last_mean_gain_ = floor_lin;
  focus_active_ = true;
  confidence_ = 1.0F;
  estimator_hold_ = false;
  init_hops_remaining_ = static_cast<std::size_t>(std::ceil(kInitSeconds * hop_hz));
  ready_ = true;
  return true;
}

void SpectralPostfilter::reset() noexcept
{
  stft_.reset();
  tracker_.reset();
  wiener_.reset();
  std::fill(power_.begin(), power_.end(), 0.0F);
  std::fill(gains_.begin(), gains_.end(), wiener_.gainFloor());
  last_mean_gain_ = wiener_.gainFloor();
  estimator_hold_ = false;
  const double hop_hz = stft_.hopSize() == 0 ? 0.0 : stft_.sampleRate() / static_cast<double>(stft_.hopSize());
  init_hops_remaining_ = hop_hz > 0.0 ? static_cast<std::size_t>(std::ceil(kInitSeconds * hop_hz)) : 0;
}

void SpectralPostfilter::setControl(const bool focus_active, const float confidence) noexcept
{
  focus_active_ = focus_active;
  confidence_ = std::clamp(confidence, 0.0F, 1.0F);
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

  bool presence = false;
  std::size_t hits = 0;
  if (tracker_.smoothedPower().size() == n_bins && tracker_.noisePower().size() == n_bins)
  {
    for (std::size_t k = 0; k < n_bins; ++k)
    {
      const float lambda = std::max(tracker_.noisePower()[k], kEps);
      if ((tracker_.smoothedPower()[k] / lambda) > kPresenceSnr)
      {
        ++hits;
      }
    }
    presence = hits >= kPresenceBinHits;
  }

  const bool focused = focus_active_ && (confidence_ >= kConfidenceThreshold);
  bool allow_noise = !estimator_hold_;
  if (init_hops_remaining_ > 0)
  {
    --init_hops_remaining_;
  }
  else if (presence)
  {
    allow_noise = false;
  }
  tracker_.update(power_, allow_noise);

  if (!focused)
  {
    std::fill(gains_.begin(), gains_.end(), 1.0F);
    last_mean_gain_ = 1.0F;
  }
  else
  {
    wiener_.compute(power_, tracker_.noisePower(), gains_);
    last_mean_gain_ = wiener_.meanGain();
  }

  for (std::size_t k = 0; k < n_bins; ++k)
  {
    const float g = std::clamp(Sanitize(gains_[k]), 0.0F, 1.0F);
    re[k] *= g;
    im[k] *= g;
    if (k != 0 && k * 2U != fft_size)
    {
      const std::size_t ck = fft_size - k;
      re[ck] *= g;
      im[ck] *= g;
    }
  }
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
  // Sanitize in place via output, then STFT from output if sizes match? Copy through
  // a hop of sanitised samples using the STFT input path: write sanitised values
  // into a stack-sized temp is unbounded. Sanitize into output first, then process
  // output as input only if we have a second buffer. Use STFT process from a local
  // sanitised view: we overwrite output after STFT, so sanitise into output then
  // we need the original. Process sample-wise sanitising by mutating a copy in output
  // then calling STFT(output, output) is wrong.
  // Sanitize into power_ scratch is too small. Use time path: STFT reads input
  // directly — sanitize in the hop callback and here by writing a cleaned copy
  // into `gains_`? No.
  // Pre-sanitize using ola scratch is internal. Simplest: if any non-finite, write
  // cleaned samples into `power_` only works for n_bins.
  // Allocate-free: reuse `gains_` is n_bins. Must use STFT's contract: pass input
  // and have process() sanitize each sample into a one-sample... 
  // We'll sanitize by copying through output if input.data()!=output.data().
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
  // Measured first-arrival for this STFT convention is fft_size-1.
  const std::size_t n = stft_.fftSize();
  return n == 0 ? 0 : n - 1U;
}

std::size_t SpectralPostfilter::persistentBytes() const noexcept
{
  return stft_.persistentBytes() +
         ((power_.size() + gains_.size()) * sizeof(float));
}
}  // namespace sonitude::dsp
