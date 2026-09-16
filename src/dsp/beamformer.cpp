#include "dsp/beamformer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <unordered_map>

#include "dsp/spectral_postfilter.hpp"
#include "spatial/angles.hpp"

namespace sonitude::dsp
{
namespace
{
constexpr std::size_t kFftSize = 128;
constexpr std::size_t kHopSize = 32;
constexpr std::size_t kM = audio::kMicChannels;
constexpr float kCovFloor = 1.0e-4F;
constexpr double kSteeringDeadbandDeg = 0.5;

struct Cpx
{
  float re = 0.0F;
  float im = 0.0F;
};

Cpx Mul(const Cpx a, const Cpx b)
{
  return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

Cpx Conj(const Cpx a)
{
  return {a.re, -a.im};
}

Cpx SteeringEntry(const double delay_samples, const std::size_t bin, const std::size_t fft_size)
{
  const double phase = 2.0 * spatial::kPi * static_cast<double>(bin) * delay_samples /
                       static_cast<double>(fft_size);
  return {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
}

struct CholFactor
{
  Cpx L[kM][kM]{};
  bool ok = false;
};

bool FactorHermitianPd(const Cpx r[kM][kM], CholFactor& factor)
{
  factor = {};
  for (std::size_t i = 0; i < kM; ++i)
  {
    double diag = static_cast<double>(r[i][i].re);
    for (std::size_t k = 0; k < i; ++k)
    {
      const Cpx lik = factor.L[i][k];
      diag -= (static_cast<double>(lik.re) * lik.re) + (static_cast<double>(lik.im) * lik.im);
    }
    if (!(diag > 1.0e-18) || !std::isfinite(diag))
    {
      return false;
    }
    const float diag_s = static_cast<float>(std::sqrt(diag));
    factor.L[i][i] = {diag_s, 0.0F};
    for (std::size_t j = i + 1U; j < kM; ++j)
    {
      double sr = static_cast<double>(r[j][i].re);
      double si = static_cast<double>(r[j][i].im);
      for (std::size_t k = 0; k < i; ++k)
      {
        const Cpx ljk = factor.L[j][k];
        const Cpx lik = factor.L[i][k];
        sr -= (static_cast<double>(ljk.re) * lik.re) + (static_cast<double>(ljk.im) * lik.im);
        si -= (static_cast<double>(ljk.im) * lik.re) - (static_cast<double>(ljk.re) * lik.im);
      }
      factor.L[j][i] = {static_cast<float>(sr / diag_s), static_cast<float>(si / diag_s)};
    }
  }
  factor.ok = true;
  return true;
}

bool SolveChol(const CholFactor& factor, const Cpx d[kM], Cpx q[kM])
{
  if (!factor.ok)
  {
    return false;
  }
  Cpx y[kM]{};
  for (std::size_t i = 0; i < kM; ++i)
  {
    double yr = static_cast<double>(d[i].re);
    double yi = static_cast<double>(d[i].im);
    for (std::size_t k = 0; k < i; ++k)
    {
      const Cpx lik = factor.L[i][k];
      yr -= (static_cast<double>(lik.re) * y[k].re) - (static_cast<double>(lik.im) * y[k].im);
      yi -= (static_cast<double>(lik.re) * y[k].im) + (static_cast<double>(lik.im) * y[k].re);
    }
    const float diag = factor.L[i][i].re;
    if (!(std::fabs(diag) > 1.0e-12F))
    {
      return false;
    }
    y[i] = {static_cast<float>(yr / diag), static_cast<float>(yi / diag)};
  }
  for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(kM) - 1; i >= 0; --i)
  {
    const std::size_t ui = static_cast<std::size_t>(i);
    double qr = static_cast<double>(y[ui].re);
    double qi = static_cast<double>(y[ui].im);
    for (std::size_t k = ui + 1U; k < kM; ++k)
    {
      const Cpx lki = factor.L[k][ui];
      qr -= (static_cast<double>(lki.re) * q[k].re) + (static_cast<double>(lki.im) * q[k].im);
      qi -= (static_cast<double>(lki.re) * q[k].im) - (static_cast<double>(lki.im) * q[k].re);
    }
    const float diag = factor.L[ui][ui].re;
    q[ui] = {static_cast<float>(qr / diag), static_cast<float>(qi / diag)};
  }
  return true;
}

void ApplyWeights(const Cpx w[kM], const Cpx x[kM], Cpx& y)
{
  Cpx acc{0.0F, 0.0F};
  for (std::size_t ch = 0; ch < kM; ++ch)
  {
    const Cpx term = Mul(Conj(w[ch]), x[ch]);
    acc.re += term.re;
    acc.im += term.im;
  }
  y = acc;
}

bool WeightsFromFactor(const CholFactor& factor,
                       const Cpx d[kM],
                       Cpx w[kM],
                       std::uint64_t& solve_count)
{
  Cpx q[kM]{};
  ++solve_count;
  if (!SolveChol(factor, d, q))
  {
    return false;
  }
  Cpx denom{0.0F, 0.0F};
  for (std::size_t ch = 0; ch < kM; ++ch)
  {
    const Cpx term = Mul(Conj(d[ch]), q[ch]);
    denom.re += term.re;
    denom.im += term.im;
  }
  const float mag2 = (denom.re * denom.re) + (denom.im * denom.im);
  if (!(mag2 > 1.0e-16F))
  {
    return false;
  }
  const float inv = 1.0F / mag2;
  const Cpx inv_d{denom.re * inv, -denom.im * inv};
  for (std::size_t ch = 0; ch < kM; ++ch)
  {
    w[ch] = Mul(q[ch], inv_d);
  }
  return true;
}

void FormLookOutput(const Cpx r_in[kM][kM],
                    const float load0,
                    const float max_wng,
                    const Cpx d[kM],
                    const Cpx x[kM],
                    Cpx& y,
                    std::uint64_t& factorization_count,
                    std::uint64_t& solve_count)
{
  Cpx r[kM][kM]{};
  for (std::size_t i = 0; i < kM; ++i)
  {
    for (std::size_t j = 0; j < kM; ++j)
    {
      r[i][j] = r_in[i][j];
    }
    r[i][i].re += load0;
    r[i][i].im = 0.0F;
  }
  CholFactor factor{};
  ++factorization_count;
  Cpx w[kM]{};
  if (!FactorHermitianPd(r, factor) || !WeightsFromFactor(factor, d, w, solve_count))
  {
    y = {0.0F, 0.0F};
    return;
  }
  (void)max_wng;
  ApplyWeights(w, x, y);
}

void ReconstructEar(const Cpx d_ear, const Cpx z, Cpx& y)
{
  y = Mul(d_ear, z);
}
}  // namespace

void MvdrBeamformer::configure(const app::GeometryConfig& geometry,
                               const app::SteeringConfig& steering_config,
                               const app::CalibrationConfig& calibration,
                               const std::uint32_t sample_rate_hz,
                               const std::size_t max_block_frames,
                               const MvdrTuningParams& tuning)
{
  app::ValidateGeometryConfig(geometry);
  if (geometry.microphones.size() != audio::kMicChannels)
  {
    throw std::runtime_error("geometry must contain exactly six microphones");
  }
  if (calibration.channels.size() != audio::kMicChannels)
  {
    throw std::runtime_error("calibration must contain exactly six channels");
  }
  if (sample_rate_hz == 0)
  {
    throw std::runtime_error("sample_rate_hz must be non-zero");
  }
  if (!std::isfinite(tuning.diag_load) || tuning.diag_load <= 0.0F || tuning.diag_load > 1.0F ||
      !std::isfinite(tuning.max_white_noise_gain) || tuning.max_white_noise_gain < 1.0F ||
      tuning.max_white_noise_gain > 32.0F || !std::isfinite(tuning.cov_tau_sec) ||
      tuning.cov_tau_sec < 0.010F || tuning.cov_tau_sec > 2.0F)
  {
    throw std::runtime_error("MVDR tuning must come from spatial.mvdr and stay in validated ranges");
  }

  steering_config_ = steering_config;
  binaural_output_ = steering_config_.experimental_dual_reference_mvdr;
  sample_rate_hz_ = sample_rate_hz;
  setTuning(tuning);
  ramp_samples_ = std::max<std::size_t>(
      1U, static_cast<std::size_t>((steering_config_.steering_ramp_ms * 0.001F) *
                                   static_cast<float>(sample_rate_hz_)));

  const std::size_t ref = steering_config_.reference_mic_index;
  if (ref >= audio::kMicChannels)
  {
    throw std::runtime_error("reference_mic_index out of range");
  }
  if (steering_config_.left_ear_mic_index >= audio::kMicChannels ||
      steering_config_.right_ear_mic_index >= audio::kMicChannels)
  {
    throw std::runtime_error("ear mic index out of range");
  }

  steering_model_.configure(geometry, steering_config_, sample_rate_hz_);

  std::unordered_map<std::string, float> cal_by_id;
  cal_by_id.reserve(calibration.channels.size());
  for (const app::CalibrationChannel& channel : calibration.channels)
  {
    cal_by_id[channel.id] = channel.delay_samples;
  }

  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const auto& mic = geometry.microphones[i];
    const auto it = cal_by_id.find(mic.id);
    if (it == cal_by_id.end())
    {
      throw std::runtime_error("geometry/calibration id mismatch");
    }
    calibration_delays_[i] = static_cast<double>(it->second);
  }

  const std::size_t fifo_frames = std::max<std::size_t>(max_block_frames, 64U);
  const StreamingStftConfig analysis{.fft_size = kFftSize, .hop_size = kHopSize, .synthesize = false};
  const StreamingStftConfig synthesis{.fft_size = kFftSize, .hop_size = kHopSize, .synthesize = true};
  const double sr = static_cast<double>(sample_rate_hz_);
  for (std::size_t ch = 0; ch < audio::kMicChannels; ++ch)
  {
    if (!mic_stft_[ch].prepare(sr, fifo_frames, analysis))
    {
      throw std::runtime_error("MVDR analysis STFT prepare failed");
    }
    mic_ctx_[ch] = {this, ch};
    x_re_[ch].assign(kFftSize, 0.0F);
    x_im_[ch].assign(kFftSize, 0.0F);
  }
  if (!target_stft_.prepare(sr, fifo_frames, synthesis) ||
      !pending_stft_.prepare(sr, fifo_frames, synthesis))
  {
    throw std::runtime_error("MVDR synthesis STFT prepare failed");
  }
  if (binaural_output_)
  {
    if (!left_target_stft_.prepare(sr, fifo_frames, synthesis) ||
        !left_pending_stft_.prepare(sr, fifo_frames, synthesis) ||
        !right_target_stft_.prepare(sr, fifo_frames, synthesis) ||
        !right_pending_stft_.prepare(sr, fifo_frames, synthesis))
    {
      throw std::runtime_error("MVDR binaural synthesis STFT prepare failed");
    }
  }
  for (std::size_t g = 0; g < kGuardLooks; ++g)
  {
    guard_y_re_[g].assign(kFftSize, 0.0F);
    guard_y_im_[g].assign(kFftSize, 0.0F);
  }
  y_re_.assign(kFftSize, 0.0F);
  y_im_.assign(kFftSize, 0.0F);
  pending_y_re_.assign(kFftSize, 0.0F);
  pending_y_im_.assign(kFftSize, 0.0F);
  left_y_re_.assign(kFftSize, 0.0F);
  left_y_im_.assign(kFftSize, 0.0F);
  right_y_re_.assign(kFftSize, 0.0F);
  right_y_im_.assign(kFftSize, 0.0F);
  pending_left_y_re_.assign(kFftSize, 0.0F);
  pending_left_y_im_.assign(kFftSize, 0.0F);
  pending_right_y_re_.assign(kFftSize, 0.0F);
  pending_right_y_im_.assign(kFftSize, 0.0F);

  const std::size_t n_bins = (kFftSize / 2U) + 1U;
  cov_.assign(n_bins, {});
  initializeCovariance();
  current_steer_.d.assign(n_bins, {});
  pending_steer_.d.assign(n_bins, {});
  current_steer_.valid = false;
  pending_steer_.valid = false;
  for (std::size_t g = 0; g < kGuardLooks; ++g)
  {
    guard_steer_[g].d.assign(n_bins, {});
    guard_steer_[g].valid = false;
  }
  const float hop_sec = static_cast<float>(kHopSize) / static_cast<float>(sample_rate_hz_);
  cov_beta_ = 1.0F - std::exp(-hop_sec / tuning_.cov_tau_sec);

  active_target_ = {0.0F, 0.0F};
  pending_target_ = active_target_;
  current_delays_ = computeRelativeDelays(active_target_, steering_config_.reference_mic_index);
  pending_delays_ = current_delays_;
  current_left_delays_ = computeBinauralDelays(active_target_, true);
  pending_left_delays_ = current_left_delays_;
  current_right_delays_ = computeBinauralDelays(active_target_, false);
  pending_right_delays_ = current_right_delays_;
  updateGuardDelays(active_target_);
  crossfading_ = false;
  fade_cursor_ = 0;
  configured_ = true;
}

void MvdrBeamformer::resetStream() noexcept
{
  if (crossfading_)
  {
    current_delays_ = pending_delays_;
    current_left_delays_ = pending_left_delays_;
    current_right_delays_ = pending_right_delays_;
    active_target_ = pending_target_;
  }
  pending_target_ = active_target_;
  pending_delays_ = current_delays_;
  pending_left_delays_ = current_left_delays_;
  pending_right_delays_ = current_right_delays_;
  for (auto& stft : mic_stft_)
  {
    stft.reset();
  }
  target_stft_.reset();
  pending_stft_.reset();
  left_target_stft_.reset();
  left_pending_stft_.reset();
  right_target_stft_.reset();
  right_pending_stft_.reset();
  initializeCovariance();
  current_steer_.valid = false;
  pending_steer_.valid = false;
  for (auto& cache : guard_steer_)
  {
    cache.valid = false;
  }
  crossfading_ = false;
  fade_cursor_ = 0;
}

void MvdrBeamformer::initializeCovariance() noexcept
{
  for (auto& bin : cov_)
  {
    bin = {};
    for (std::size_t i = 0; i < kM; ++i)
    {
      bin[i][i] = {kCovFloor, 0.0F};
    }
  }
}

void MvdrBeamformer::EnsureSteeringCache(SteeringCache& cache, const DelayArray& delays) noexcept
{
  if (cache.valid && cache.delays == delays)
  {
    return;
  }
  cache.delays = delays;
  const std::size_t n_bins = (kFftSize / 2U) + 1U;
  if (cache.d.size() != n_bins)
  {
    cache.d.assign(n_bins, {});
  }
  for (std::size_t b = 0; b < n_bins; ++b)
  {
    for (std::size_t ch = 0; ch < kM; ++ch)
    {
      const Cpx d = SteeringEntry(delays[ch], b, kFftSize);
      cache.d[b][ch] = {d.re, d.im};
    }
  }
  cache.valid = true;
}

MvdrBeamformer::DelayArray MvdrBeamformer::computeRelativeDelays(
    const audio::BeamformerSteering target,
    const std::size_t reference_mic_index) const
{
  DelayArray geom = steering_model_.computeNearFieldDelays(target, reference_mic_index);

  const double ref_cal = calibration_delays_[reference_mic_index];
  DelayArray out{};
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    out[i] = geom[i] + calibration_delays_[i] - ref_cal;
  }
  return out;
}

MvdrBeamformer::DelayArray MvdrBeamformer::computeBinauralDelays(
    const audio::BeamformerSteering target,
    const bool left_ear) const
{
  const std::size_t ref =
      left_ear ? steering_config_.left_ear_mic_index : steering_config_.right_ear_mic_index;
  return computeRelativeDelays(target, ref);
}

audio::BeamformerSteering MvdrBeamformer::normalizeSteeringTarget(
    const audio::BeamformerSteering target) const noexcept
{
  audio::BeamformerSteering out = target;
  out.azimuth_deg =
      static_cast<float>(spatial::NormalizeAzimuthDeg(static_cast<double>(target.azimuth_deg)));
  return out;
}

bool MvdrBeamformer::steeringWithinDeadband(const audio::BeamformerSteering a,
                                            const audio::BeamformerSteering b) const noexcept
{
  return spatial::SteeringApproximatelyEqual(a.azimuth_deg,
                                             a.elevation_deg,
                                             b.azimuth_deg,
                                             b.elevation_deg,
                                             kSteeringDeadbandDeg);
}

void MvdrBeamformer::assignPendingLook(const audio::BeamformerSteering target) noexcept
{
  pending_target_ = target;
  pending_delays_ = computeRelativeDelays(target, steering_config_.reference_mic_index);
  pending_left_delays_ = computeBinauralDelays(target, true);
  pending_right_delays_ = computeBinauralDelays(target, false);
}

void MvdrBeamformer::swapActivePendingPaths() noexcept
{
  std::swap(current_delays_, pending_delays_);
  std::swap(current_left_delays_, pending_left_delays_);
  std::swap(current_right_delays_, pending_right_delays_);
  std::swap(active_target_, pending_target_);
  std::swap(current_steer_, pending_steer_);
  std::swap(target_stft_, pending_stft_);
  if (binaural_output_)
  {
    std::swap(left_target_stft_, left_pending_stft_);
    std::swap(right_target_stft_, right_pending_stft_);
  }
}

void MvdrBeamformer::pivotCrossfadeForRetarget() noexcept
{
  fade_cursor_ = ramp_samples_ - fade_cursor_;
  swapActivePendingPaths();
}

void MvdrBeamformer::completeCrossfade() noexcept
{
  crossfading_ = false;
  fade_cursor_ = 0;
  current_delays_ = pending_delays_;
  current_left_delays_ = pending_left_delays_;
  current_right_delays_ = pending_right_delays_;
  active_target_ = pending_target_;
  current_steer_ = pending_steer_;
  std::swap(target_stft_, pending_stft_);
  if (binaural_output_)
  {
    std::swap(left_target_stft_, left_pending_stft_);
    std::swap(right_target_stft_, right_pending_stft_);
  }
  updateGuardDelays(active_target_);
}

void MvdrBeamformer::setTarget(const audio::BeamformerSteering target_in)
{
  if (!configured_)
  {
    throw std::runtime_error("beamformer used before configure");
  }
  const audio::BeamformerSteering target = normalizeSteeringTarget(target_in);
  const audio::BeamformerSteering reference = crossfading_ ? pending_target_ : active_target_;
  if (steeringWithinDeadband(target, reference))
  {
    return;
  }
  const bool was_crossfading = crossfading_;
  if (was_crossfading)
  {
    pivotCrossfadeForRetarget();
  }
  else
  {
    fade_cursor_ = 0;
  }
  assignPendingLook(target);
  updateGuardDelays(pending_target_);
  crossfading_ = true;
}

void MvdrBeamformer::setTuning(const MvdrTuningParams& tuning) noexcept
{
  if (!std::isfinite(tuning.diag_load) || tuning.diag_load <= 0.0F || tuning.diag_load > 1.0F ||
      !std::isfinite(tuning.max_white_noise_gain) || tuning.max_white_noise_gain < 1.0F ||
      tuning.max_white_noise_gain > 32.0F || !std::isfinite(tuning.cov_tau_sec) ||
      tuning.cov_tau_sec < 0.010F || tuning.cov_tau_sec > 2.0F)
  {
    return;
  }
  const float tau = tuning.cov_tau_sec;
  const bool tau_changed = tau != tuning_.cov_tau_sec;
  tuning_.diag_load = tuning.diag_load;
  tuning_.max_white_noise_gain = tuning.max_white_noise_gain;
  tuning_.cov_tau_sec = tau;
  if (tau_changed && configured_ && sample_rate_hz_ > 0)
  {
    const float hop_sec = static_cast<float>(kHopSize) / static_cast<float>(sample_rate_hz_);
    cov_beta_ = 1.0F - std::exp(-hop_sec / tuning_.cov_tau_sec);
  }
}

void MvdrBeamformer::updateGuardDelays(
    const audio::BeamformerSteering& estimator_target) noexcept
{
  for (std::size_t g = 0; g < kGuardLooks; ++g)
  {
    const audio::BeamformerSteering look{
        static_cast<float>(
            spatial::NormalizeAzimuthDeg(static_cast<double>(estimator_target.azimuth_deg) +
                                         static_cast<double>(kGuardAzimuthOffsetDeg[g]))),
        estimator_target.elevation_deg};
    guard_delays_[g] = computeRelativeDelays(look, steering_config_.reference_mic_index);
  }
}

void MvdrBeamformer::OnMicHop(void* context, float* re, float* im, const std::size_t fft_size) noexcept
{
  auto* ctx = static_cast<MicHopContext*>(context);
  if (ctx == nullptr || ctx->self == nullptr)
  {
    return;
  }
  ctx->self->StoreMicSpectrum(ctx->channel, re, im, fft_size);
  if (ctx->channel + 1U == audio::kMicChannels)
  {
    ctx->self->FormLooksAndSynthesize(fft_size);
  }
}

void MvdrBeamformer::StoreMicSpectrum(const std::size_t channel,
                                      const float* re,
                                      const float* im,
                                      const std::size_t fft_size) noexcept
{
  if (channel >= audio::kMicChannels || re == nullptr || im == nullptr)
  {
    return;
  }
  std::memcpy(x_re_[channel].data(), re, fft_size * sizeof(float));
  std::memcpy(x_im_[channel].data(), im, fft_size * sizeof(float));
}

void MvdrBeamformer::ApplyHermitian(std::vector<float>& y_re,
                                    std::vector<float>& y_im,
                                    const std::size_t fft_size) const noexcept
{
  y_im[0] = 0.0F;
  if ((fft_size % 2U) == 0U)
  {
    y_im[fft_size / 2U] = 0.0F;
  }
  for (std::size_t k = 1; k < fft_size / 2U; ++k)
  {
    y_re[fft_size - k] = y_re[k];
    y_im[fft_size - k] = -y_im[k];
  }
}

void MvdrBeamformer::FormLooksAndSynthesize(const std::size_t fft_size) noexcept
{
  (void)fft_size;
  const std::size_t n_bins = (kFftSize / 2U) + 1U;
  const float keep = 1.0F - cov_beta_;
  const std::size_t left_i = steering_config_.left_ear_mic_index;
  const std::size_t right_i = steering_config_.right_ear_mic_index;

  Cpx x[kM]{};
  for (std::size_t b = 0; b < n_bins; ++b)
  {
    for (std::size_t ch = 0; ch < kM; ++ch)
    {
      x[ch] = {x_re_[ch][b], x_im_[ch][b]};
    }
    for (std::size_t i = 0; i < kM; ++i)
    {
      for (std::size_t j = 0; j < kM; ++j)
      {
        const Cpx xxh = Mul(x[i], Conj(x[j]));
        auto& rij = cov_[b][i][j];
        rij[0] = (keep * rij[0]) + (cov_beta_ * xxh.re);
        rij[1] = (keep * rij[1]) + (cov_beta_ * xxh.im);
      }
      cov_[b][i][i][0] = std::max(cov_[b][i][i][0], kCovFloor);
      cov_[b][i][i][1] = 0.0F;
    }
  }
  ++cov_update_hops_;

  EnsureSteeringCache(current_steer_, current_delays_);
  if (crossfading_)
  {
    EnsureSteeringCache(pending_steer_, pending_delays_);
  }
  if (spectral_filter_ != nullptr)
  {
    for (std::size_t g = 0; g < kGuardLooks; ++g)
    {
      EnsureSteeringCache(guard_steer_[g], guard_delays_[g]);
    }
  }

  auto load_d = [](const SteeringCache& cache, const std::size_t b, Cpx d[kM]) {
    for (std::size_t ch = 0; ch < kM; ++ch)
    {
      d[ch] = {cache.d[b][ch][0], cache.d[b][ch][1]};
    }
  };

  for (std::size_t b = 0; b < n_bins; ++b)
  {
    for (std::size_t ch = 0; ch < kM; ++ch)
    {
      x[ch] = {x_re_[ch][b], x_im_[ch][b]};
    }

    Cpx d[kM]{};
    load_d(current_steer_, b, d);
    Cpx y{};
    Cpx r[kM][kM]{};
    float trace = 0.0F;
    for (std::size_t i = 0; i < kM; ++i)
    {
      for (std::size_t j = 0; j < kM; ++j)
      {
        r[i][j] = {cov_[b][i][j][0], cov_[b][i][j][1]};
      }
      trace += r[i][i].re;
    }
    const float load = tuning_.diag_load * (trace / static_cast<float>(kM));

    FormLookOutput(r, load, tuning_.max_white_noise_gain, d, x, y, factorization_count_,
                   solve_count_);
    ++look_count_;
    y_re_[b] = y.re;
    y_im_[b] = y.im;
    if (binaural_output_)
    {
      Cpx left{};
      Cpx right{};
      ReconstructEar({d[left_i].re, d[left_i].im}, y, left);
      ReconstructEar({d[right_i].re, d[right_i].im}, y, right);
      left_y_re_[b] = left.re;
      left_y_im_[b] = left.im;
      right_y_re_[b] = right.re;
      right_y_im_[b] = right.im;
    }

    if (crossfading_)
    {
      Cpx dp[kM]{};
      load_d(pending_steer_, b, dp);
      Cpx yp{};
      FormLookOutput(r, load, tuning_.max_white_noise_gain, dp, x, yp, factorization_count_,
                     solve_count_);
      ++look_count_;
      pending_y_re_[b] = yp.re;
      pending_y_im_[b] = yp.im;
      if (binaural_output_)
      {
        Cpx left{};
        Cpx right{};
        ReconstructEar({dp[left_i].re, dp[left_i].im}, yp, left);
        ReconstructEar({dp[right_i].re, dp[right_i].im}, yp, right);
        pending_left_y_re_[b] = left.re;
        pending_left_y_im_[b] = left.im;
        pending_right_y_re_[b] = right.re;
        pending_right_y_im_[b] = right.im;
      }
    }

    if (spectral_filter_ != nullptr)
    {
      for (std::size_t g = 0; g < kGuardLooks; ++g)
      {
        Cpx dg[kM]{};
        load_d(guard_steer_[g], b, dg);
        Cpx yg{};
        FormLookOutput(r, load, tuning_.max_white_noise_gain, dg, x, yg, factorization_count_,
                       solve_count_);
        ++look_count_;
        guard_y_re_[g][b] = yg.re;
        guard_y_im_[g][b] = yg.im;
      }
    }
  }

  ApplyHermitian(y_re_, y_im_, kFftSize);
  if (binaural_output_)
  {
    ApplyHermitian(left_y_re_, left_y_im_, kFftSize);
    ApplyHermitian(right_y_re_, right_y_im_, kFftSize);
  }
  if (crossfading_)
  {
    ApplyHermitian(pending_y_re_, pending_y_im_, kFftSize);
    if (binaural_output_)
    {
      ApplyHermitian(pending_left_y_re_, pending_left_y_im_, kFftSize);
      ApplyHermitian(pending_right_y_re_, pending_right_y_im_, kFftSize);
    }
  }
  if (spectral_filter_ != nullptr)
  {
    for (std::size_t g = 0; g < kGuardLooks; ++g)
    {
      ApplyHermitian(guard_y_re_[g], guard_y_im_[g], kFftSize);
    }
    const auto guards = BindGuardSpectra(guard_y_re_, guard_y_im_);
    std::array<std::span<const float>, audio::kMicChannels> mic_re{};
    std::array<std::span<const float>, audio::kMicChannels> mic_im{};
    for (std::size_t ch = 0; ch < audio::kMicChannels; ++ch)
    {
      mic_re[ch] = x_re_[ch];
      mic_im[ch] = x_im_[ch];
    }
    spectral_filter_->updateAmplitudeProximity(mic_re, mic_im);
    if (crossfading_)
    {
      spectral_filter_->processSpectrum(pending_y_re_, pending_y_im_, guards);
      spectral_filter_->applyStoredGains(y_re_, y_im_);
    }
    else
    {
      spectral_filter_->processSpectrum(y_re_, y_im_, guards);
    }
    if (binaural_output_)
    {
      if (crossfading_)
      {
        spectral_filter_->applyStoredGains(pending_left_y_re_, pending_left_y_im_);
        spectral_filter_->applyStoredGains(pending_right_y_re_, pending_right_y_im_);
      }
      spectral_filter_->applyStoredGains(left_y_re_, left_y_im_);
      spectral_filter_->applyStoredGains(right_y_re_, right_y_im_);
    }
  }
  target_stft_.overlapAddSpectrum(y_re_.data(), y_im_.data());
  if (binaural_output_)
  {
    left_target_stft_.overlapAddSpectrum(left_y_re_.data(), left_y_im_.data());
    right_target_stft_.overlapAddSpectrum(right_y_re_.data(), right_y_im_.data());
  }
  if (crossfading_)
  {
    pending_stft_.overlapAddSpectrum(pending_y_re_.data(), pending_y_im_.data());
    if (binaural_output_)
    {
      left_pending_stft_.overlapAddSpectrum(pending_left_y_re_.data(), pending_left_y_im_.data());
      right_pending_stft_.overlapAddSpectrum(pending_right_y_re_.data(), pending_right_y_im_.data());
    }
  }
  else
  {
    pending_stft_.overlapAddSpectrum(y_re_.data(), y_im_.data());
    if (binaural_output_)
    {
      left_pending_stft_.overlapAddSpectrum(left_y_re_.data(), left_y_im_.data());
      right_pending_stft_.overlapAddSpectrum(right_y_re_.data(), right_y_im_.data());
    }
  }
}

float MvdrBeamformer::PopMono()
{
  auto blend = [&](const float current, const float pending) -> float {
    if (!crossfading_)
    {
      return current;
    }
    const float alpha =
        static_cast<float>(fade_cursor_) / static_cast<float>(std::max<std::size_t>(1U, ramp_samples_));
    return ((1.0F - alpha) * current) + (alpha * pending);
  };
  return blend(target_stft_.pop(), pending_stft_.pop());
}

float MvdrBeamformer::PopEar(const bool left_channel)
{
  auto blend = [&](const float current, const float pending) -> float {
    if (!crossfading_)
    {
      return current;
    }
    const float alpha =
        static_cast<float>(fade_cursor_) / static_cast<float>(std::max<std::size_t>(1U, ramp_samples_));
    return ((1.0F - alpha) * current) + (alpha * pending);
  };
  if (left_channel)
  {
    return blend(left_target_stft_.pop(), left_pending_stft_.pop());
  }
  return blend(right_target_stft_.pop(), right_pending_stft_.pop());
}

void MvdrBeamformer::process(const std::span<const audio::MicFrame> input,
                             const std::span<float> mono_out)
{
  if (!configured_)
  {
    throw std::runtime_error("beamformer used before configure");
  }
  if (mono_out.size() < input.size())
  {
    throw std::runtime_error("mono_out span too small for input");
  }

  for (std::size_t i = 0; i < input.size(); ++i)
  {
    for (std::size_t ch = 0; ch < audio::kMicChannels; ++ch)
    {
      mic_stft_[ch].feed(input[i][ch], &MvdrBeamformer::OnMicHop, &mic_ctx_[ch]);
    }
    mono_out[i] = PopMono();
    if (binaural_output_)
    {
      (void)PopEar(true);
      (void)PopEar(false);
    }
    if (crossfading_)
    {
      ++fade_cursor_;
      if (fade_cursor_ >= ramp_samples_)
      {
        completeCrossfade();
      }
    }
  }
}

void MvdrBeamformer::processStereo(const std::span<const audio::MicFrame> input,
                                   const std::span<float> left_out,
                                   const std::span<float> right_out)
{
  if (!configured_ || !binaural_output_)
  {
    throw std::runtime_error("processStereo requires binaural MVDR output");
  }
  if (left_out.size() < input.size() || right_out.size() < input.size())
  {
    throw std::runtime_error("stereo output spans too small for input");
  }

  for (std::size_t i = 0; i < input.size(); ++i)
  {
    for (std::size_t ch = 0; ch < audio::kMicChannels; ++ch)
    {
      mic_stft_[ch].feed(input[i][ch], &MvdrBeamformer::OnMicHop, &mic_ctx_[ch]);
    }
    (void)PopMono();
    left_out[i] = PopEar(true);
    right_out[i] = PopEar(false);
    if (crossfading_)
    {
      ++fade_cursor_;
      if (fade_cursor_ >= ramp_samples_)
      {
        completeCrossfade();
      }
    }
  }
}

std::size_t MvdrBeamformer::algorithmicDelaySamples() const noexcept
{
  return configured_ ? (kFftSize - 1U) : 0U;
}
}  // namespace sonitude::dsp
