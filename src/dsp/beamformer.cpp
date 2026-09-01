#include "dsp/beamformer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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

bool SolveRwEqualsD(const Cpx r[kM][kM], const Cpx d[kM], Cpx w[kM])
{
  constexpr std::size_t kN = 2U * kM;
  double a[kN][kN + 1U]{};
  for (std::size_t i = 0; i < kM; ++i)
  {
    for (std::size_t j = 0; j < kM; ++j)
    {
      a[i][j] = static_cast<double>(r[i][j].re);
      a[i][j + kM] = static_cast<double>(-r[i][j].im);
      a[i + kM][j] = static_cast<double>(r[i][j].im);
      a[i + kM][j + kM] = static_cast<double>(r[i][j].re);
    }
    a[i][kN] = static_cast<double>(d[i].re);
    a[i + kM][kN] = static_cast<double>(d[i].im);
  }
  for (std::size_t col = 0; col < kN; ++col)
  {
    std::size_t pivot = col;
    double best = std::fabs(a[col][col]);
    for (std::size_t row = col + 1U; row < kN; ++row)
    {
      const double mag = std::fabs(a[row][col]);
      if (mag > best)
      {
        best = mag;
        pivot = row;
      }
    }
    if (!(best > 1.0e-12))
    {
      return false;
    }
    if (pivot != col)
    {
      for (std::size_t j = col; j <= kN; ++j)
      {
        std::swap(a[col][j], a[pivot][j]);
      }
    }
    const double diag = a[col][col];
    for (std::size_t j = col; j <= kN; ++j)
    {
      a[col][j] /= diag;
    }
    for (std::size_t row = 0; row < kN; ++row)
    {
      if (row == col)
      {
        continue;
      }
      const double f = a[row][col];
      if (f == 0.0)
      {
        continue;
      }
      for (std::size_t j = col; j <= kN; ++j)
      {
        a[row][j] -= f * a[col][j];
      }
    }
  }
  for (std::size_t i = 0; i < kM; ++i)
  {
    w[i] = {static_cast<float>(a[i][kN]), static_cast<float>(a[i + kM][kN])};
  }
  return true;
}

void DelayAndSum(const Cpx d[kM], const Cpx x[kM], Cpx& y)
{
  Cpx acc{0.0F, 0.0F};
  for (std::size_t ch = 0; ch < kM; ++ch)
  {
    const Cpx term = Mul(Conj(d[ch]), x[ch]);
    acc.re += term.re;
    acc.im += term.im;
  }
  y = {acc.re / static_cast<float>(kM), acc.im / static_cast<float>(kM)};
}

void MvdrCombine(const Cpx r[kM][kM],
                 const Cpx d[kM],
                 const Cpx x[kM],
                 Cpx& y,
                 const float max_white_noise_gain)
{
  Cpx w[kM]{};
  if (!SolveRwEqualsD(r, d, w))
  {
    DelayAndSum(d, x, y);
    return;
  }
  Cpx denom{0.0F, 0.0F};
  for (std::size_t ch = 0; ch < kM; ++ch)
  {
    const Cpx term = Mul(Conj(d[ch]), w[ch]);
    denom.re += term.re;
    denom.im += term.im;
  }
  const float mag2 = (denom.re * denom.re) + (denom.im * denom.im);
  if (!(mag2 > 1.0e-16F))
  {
    DelayAndSum(d, x, y);
    return;
  }
  const float inv = 1.0F / mag2;
  const Cpx inv_d{denom.re * inv, -denom.im * inv};
  float wn = 0.0F;
  for (std::size_t ch = 0; ch < kM; ++ch)
  {
    w[ch] = Mul(w[ch], inv_d);
    wn += (w[ch].re * w[ch].re) + (w[ch].im * w[ch].im);
  }
  Cpx unity{0.0F, 0.0F};
  for (std::size_t ch = 0; ch < kM; ++ch)
  {
    const Cpx term = Mul(Conj(d[ch]), w[ch]);
    unity.re += term.re;
    unity.im += term.im;
  }
  const float ds_wn = 1.0F / static_cast<float>(kM);
  if (wn > max_white_noise_gain * ds_wn || std::fabs(unity.re - 1.0F) > 0.25F ||
      std::fabs(unity.im) > 0.25F)
  {
    DelayAndSum(d, x, y);
    return;
  }
  Cpx acc{0.0F, 0.0F};
  for (std::size_t ch = 0; ch < kM; ++ch)
  {
    const Cpx term = Mul(Conj(w[ch]), x[ch]);
    acc.re += term.re;
    acc.im += term.im;
  }
  y = acc;
}
}  // namespace

void MvdrBeamformer::configure(const app::GeometryConfig& geometry,
                               const app::SteeringConfig& steering_config,
                               const app::CalibrationConfig& calibration,
                               const std::uint32_t sample_rate_hz,
                               const std::size_t max_block_frames,
                               const HrtfTable* kemar_table)
{
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

  steering_config_ = steering_config;
  near_field_ = steering_config_.model != "far_field";
  binaural_output_ = steering_config_.binaural_output;
  sample_rate_hz_ = sample_rate_hz;
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

  if (steering_config_.kemar_lut.enabled)
  {
    if (kemar_table == nullptr || kemar_table->empty())
    {
      throw std::runtime_error("KEMAR steering LUT enabled but HRTF table is missing");
    }
    steering_model_.configure(*kemar_table, geometry, steering_config_, sample_rate_hz_);
  }
  else
  {
    steering_model_.configureAnalytic(geometry, steering_config_, sample_rate_hz_);
  }

  std::unordered_map<std::string, float> cal_by_id;
  cal_by_id.reserve(calibration.channels.size());
  for (const app::CalibrationChannel& channel : calibration.channels)
  {
    cal_by_id[channel.id] = channel.delay_samples;
  }

  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const auto& mic = geometry.microphones[i];
    mic_positions_[i] = {mic.x, mic.y, mic.z};
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
  for (std::size_t b = 0; b < n_bins; ++b)
  {
    for (std::size_t i = 0; i < kM; ++i)
    {
      cov_[b][i][i] = {kCovFloor, 0.0F};
    }
  }
  const float hop_sec = static_cast<float>(kHopSize) / static_cast<float>(sample_rate_hz_);
  cov_beta_ = 1.0F - std::exp(-hop_sec / std::max(tuning_.cov_tau_sec, 1.0e-3F));

  current_target_ = {0.0F, 0.0F};
  current_delays_ = computeRelativeDelays(current_target_, steering_config_.reference_mic_index);
  pending_delays_ = current_delays_;
  current_left_delays_ = computeBinauralDelays(current_target_, true);
  pending_left_delays_ = current_left_delays_;
  current_right_delays_ = computeBinauralDelays(current_target_, false);
  pending_right_delays_ = current_right_delays_;
  updateGuardDelays();
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
  }
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
  for (auto& bin : cov_)
  {
    bin = {};
  }
  crossfading_ = false;
  fade_cursor_ = 0;
}

MvdrBeamformer::DelayArray MvdrBeamformer::computeRelativeDelays(
    const audio::BeamformerSteering target,
    const std::size_t reference_mic_index) const
{
  audio::BeamformerSteering look = target;
  DelayArray geom{};
  if (near_field_)
  {
    if (steering_model_.enabled())
    {
      const std::size_t idx =
          steering_model_.lookupIndex(target.azimuth_deg, target.elevation_deg);
      look = steering_model_.snappedDirection(idx);
      if (reference_mic_index == steering_config_.reference_mic_index)
      {
        geom = steering_model_.delaysForIndex(idx);
      }
      else
      {
        geom = steering_model_.computeNearFieldDelays(look, reference_mic_index);
      }
    }
    else
    {
      geom = steering_model_.computeNearFieldDelays(look, reference_mic_index);
    }
  }
  else
  {
    const auto u = spatial::UnitVectorFromAzElDeg(look.azimuth_deg, look.elevation_deg);
    const double ref_dot = (u[0] * mic_positions_[reference_mic_index][0]) +
                           (u[1] * mic_positions_[reference_mic_index][1]) +
                           (u[2] * mic_positions_[reference_mic_index][2]);
    for (std::size_t i = 0; i < audio::kMicChannels; ++i)
    {
      const double dot = (u[0] * mic_positions_[i][0]) + (u[1] * mic_positions_[i][1]) +
                         (u[2] * mic_positions_[i][2]);
      const double tau_sec =
          -((dot - ref_dot) / static_cast<double>(steering_config_.speed_of_sound_mps));
      geom[i] = tau_sec * static_cast<double>(sample_rate_hz_);
    }
  }

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
  DelayArray out = computeRelativeDelays(target, ref);
  if (steering_model_.enabled())
  {
    const std::size_t idx = steering_model_.lookupIndex(target.azimuth_deg, target.elevation_deg);
    const float offset = left_ear ? steering_model_.leftEarOffsetSamples(idx)
                                  : steering_model_.rightEarOffsetSamples(idx);
    for (double& delay : out)
    {
      delay += static_cast<double>(offset);
    }
  }
  return out;
}

void MvdrBeamformer::setTarget(const audio::BeamformerSteering target)
{
  if (!configured_)
  {
    throw std::runtime_error("beamformer used before configure");
  }
  if (spatial::SteeringApproximatelyEqual(target.azimuth_deg,
                                          target.elevation_deg,
                                          current_target_.azimuth_deg,
                                          current_target_.elevation_deg))
  {
    return;
  }
  pending_delays_ = computeRelativeDelays(target, steering_config_.reference_mic_index);
  pending_left_delays_ = computeBinauralDelays(target, true);
  pending_right_delays_ = computeBinauralDelays(target, false);
  current_target_ = target;
  updateGuardDelays();
  crossfading_ = true;
  fade_cursor_ = 0;
}

void MvdrBeamformer::setTuning(const MvdrTuningParams& tuning) noexcept
{
  tuning_.diag_load = std::clamp(tuning.diag_load, 0.001F, 1.0F);
  tuning_.max_white_noise_gain = std::clamp(tuning.max_white_noise_gain, 1.0F, 32.0F);
  tuning_.cov_tau_sec = std::clamp(tuning.cov_tau_sec, 0.010F, 2.0F);
  if (configured_ && sample_rate_hz_ > 0)
  {
    const float hop_sec = static_cast<float>(kHopSize) / static_cast<float>(sample_rate_hz_);
    cov_beta_ = 1.0F - std::exp(-hop_sec / tuning_.cov_tau_sec);
  }
}

void MvdrBeamformer::updateGuardDelays()
{
  for (std::size_t g = 0; g < kGuardLooks; ++g)
  {
    const audio::BeamformerSteering look{
        static_cast<float>(
            spatial::NormalizeAzimuthDeg(static_cast<double>(current_target_.azimuth_deg) +
                                         static_cast<double>(kGuardAzimuthOffsetDeg[g]))),
        current_target_.elevation_deg};
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

void MvdrBeamformer::FormLookSpectrum(const DelayArray& delays,
                                      std::vector<float>& y_re,
                                      std::vector<float>& y_im) const noexcept
{
  const std::size_t fft_size = kFftSize;
  const std::size_t n_bins = (fft_size / 2U) + 1U;
  Cpx x[kM]{};
  Cpx d[kM]{};
  Cpx r[kM][kM]{};

  for (std::size_t b = 0; b < n_bins; ++b)
  {
    for (std::size_t ch = 0; ch < kM; ++ch)
    {
      x[ch] = {x_re_[ch][b], x_im_[ch][b]};
      d[ch] = SteeringEntry(delays[ch], b, fft_size);
    }
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
    for (std::size_t i = 0; i < kM; ++i)
    {
      r[i][i].re += load;
      r[i][i].im = 0.0F;
    }
    Cpx y{};
    if (b == 0U || b + 1U == n_bins)
    {
      DelayAndSum(d, x, y);
    }
    else
    {
      MvdrCombine(r, d, x, y, tuning_.max_white_noise_gain);
    }
    y_re[b] = y.re;
    y_im[b] = y.im;
  }
  ApplyHermitian(y_re, y_im, fft_size);
}

void MvdrBeamformer::FormLooksAndSynthesize(const std::size_t fft_size) noexcept
{
  (void)fft_size;
  const std::size_t n_bins = (kFftSize / 2U) + 1U;
  const float keep = 1.0F - cov_beta_;

  Cpx x[kM]{};
  if (!crossfading_)
  {
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
        cov_[b][i][i][1] = 0.0F;
      }
    }
  }

  FormLookSpectrum(current_delays_, y_re_, y_im_);
  if (binaural_output_)
  {
    FormLookSpectrum(current_left_delays_, left_y_re_, left_y_im_);
    FormLookSpectrum(current_right_delays_, right_y_re_, right_y_im_);
  }
  if (crossfading_)
  {
    FormLookSpectrum(pending_delays_, pending_y_re_, pending_y_im_);
    if (binaural_output_)
    {
      FormLookSpectrum(pending_left_delays_, pending_left_y_re_, pending_left_y_im_);
      FormLookSpectrum(pending_right_delays_, pending_right_y_re_, pending_right_y_im_);
    }
  }
  if (spectral_filter_ != nullptr)
  {
    for (std::size_t g = 0; g < kGuardLooks; ++g)
    {
      FormLookSpectrum(guard_delays_[g], guard_y_re_[g], guard_y_im_[g]);
    }
    const auto guards = BindGuardSpectra(guard_y_re_, guard_y_im_);
    if (crossfading_)
    {
      spectral_filter_->processSpectrum(pending_y_re_, pending_y_im_, guards);
      spectral_filter_->applyStoredGains(y_re_, y_im_);
    }
    else
    {
      spectral_filter_->processSpectrum(y_re_, y_im_, guards);
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
        crossfading_ = false;
        fade_cursor_ = 0;
        current_delays_ = pending_delays_;
        current_left_delays_ = pending_left_delays_;
        current_right_delays_ = pending_right_delays_;
        std::swap(target_stft_, pending_stft_);
        if (binaural_output_)
        {
          std::swap(left_target_stft_, left_pending_stft_);
          std::swap(right_target_stft_, right_pending_stft_);
        }
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
        crossfading_ = false;
        fade_cursor_ = 0;
        current_delays_ = pending_delays_;
        current_left_delays_ = pending_left_delays_;
        current_right_delays_ = pending_right_delays_;
        std::swap(target_stft_, pending_stft_);
        std::swap(left_target_stft_, left_pending_stft_);
        std::swap(right_target_stft_, right_pending_stft_);
      }
    }
  }
}

std::size_t MvdrBeamformer::algorithmicDelaySamples() const noexcept
{
  return configured_ ? (kFftSize - 1U) : 0U;
}
}  // namespace sonitude::dsp
