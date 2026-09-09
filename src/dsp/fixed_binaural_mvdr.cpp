#include "dsp/fixed_binaural_mvdr.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "spatial/angles.hpp"

namespace sonitude::dsp
{
namespace
{
constexpr std::size_t kFftSize = 128;
constexpr std::size_t kHopSize = 32;
constexpr std::size_t kM = audio::kMicChannels;
constexpr double kSteeringDeadbandDeg = 0.5;

float DbToLin(const float db)
{
  return std::pow(10.0F, db / 10.0F);
}

float Smoothstep01(const float x)
{
  const float t = std::clamp(x, 0.0F, 1.0F);
  return t * t * (3.0F - (2.0F * t));
}
}  // namespace

void FixedBinauralMvdr::configure(const ArrayProfile& profile,
                                  const std::uint32_t sample_rate_hz,
                                  const std::size_t max_block_frames,
                                  const FixedMvdrMaskParams& mask)
{
  ValidateArrayProfile(profile, sample_rate_hz, static_cast<std::uint16_t>(kFftSize),
                       static_cast<std::uint16_t>(kHopSize));
  if (profile.bin_count != kPositiveBinCount128)
  {
    throw std::runtime_error("FixedBinauralMvdr requires 65 positive-frequency bins");
  }
  profile_ = profile;
  mask_ = mask;
  sample_rate_hz_ = sample_rate_hz;
  ramp_samples_ = std::max<std::size_t>(1U, static_cast<std::size_t>(0.150F * sample_rate_hz_));
  const std::size_t fifo_frames = std::max<std::size_t>(max_block_frames, 64U);
  const StreamingStftConfig analysis{.fft_size = kFftSize, .hop_size = kHopSize, .synthesize = false};
  const StreamingStftConfig synthesis{.fft_size = kFftSize, .hop_size = kHopSize, .synthesize = true};
  const double sr = static_cast<double>(sample_rate_hz_);
  for (std::size_t ch = 0; ch < kM; ++ch)
  {
    if (!mic_stft_[ch].prepare(sr, fifo_frames, analysis))
    {
      throw std::runtime_error("fixed MVDR analysis STFT prepare failed");
    }
    mic_ctx_[ch] = {this, ch};
    x_re_[ch].assign(kFftSize, 0.0F);
    x_im_[ch].assign(kFftSize, 0.0F);
  }
  if (!left_stft_.prepare(sr, fifo_frames, synthesis) || !right_stft_.prepare(sr, fifo_frames, synthesis))
  {
    throw std::runtime_error("fixed MVDR synthesis STFT prepare failed");
  }
  left_y_re_.assign(kFftSize, 0.0F);
  left_y_im_.assign(kFftSize, 0.0F);
  right_y_re_.assign(kFftSize, 0.0F);
  right_y_im_.assign(kFftSize, 0.0F);
  pending_left_re_.assign(kFftSize, 0.0F);
  pending_left_im_.assign(kFftSize, 0.0F);
  pending_right_re_.assign(kFftSize, 0.0F);
  pending_right_im_.assign(kFftSize, 0.0F);
  pz_smooth_.assign(profile_.bin_count, 0.0F);
  pr_smooth_.assign(profile_.bin_count, 0.0F);
  const float hop_sec = static_cast<float>(kHopSize) / static_cast<float>(sample_rate_hz_);
  const float tau = std::max(mask_.smooth_sec, hop_sec);
  mask_alpha_ = 1.0F - std::exp(-hop_sec / tau);
  active_azimuth_ = 0.0F;
  pending_azimuth_ = 0.0F;
  active_dir_ = NearestAzimuthIndex(profile_, 0.0F);
  pending_dir_ = active_dir_;
  crossfading_ = false;
  fade_cursor_ = 0;
  have_queued_ = false;
  configured_ = true;
}

void FixedBinauralMvdr::setTarget(const audio::BeamformerSteering target)
{
  if (!configured_)
  {
    throw std::runtime_error("fixed MVDR used before configure");
  }
  const float az =
      static_cast<float>(spatial::NormalizeAzimuthDeg(static_cast<double>(target.azimuth_deg)));
  const float reference = crossfading_ ? pending_azimuth_ : active_azimuth_;
  const double d = std::fabs(spatial::NormalizeAzimuthDeg(static_cast<double>(az - reference)));
  if (d < kSteeringDeadbandDeg)
  {
    return;
  }
  if (crossfading_)
  {
    queued_azimuth_ = az;
    have_queued_ = true;
    return;
  }
  pending_azimuth_ = az;
  pending_dir_ = NearestAzimuthIndex(profile_, az);
  fade_cursor_ = 0;
  crossfading_ = pending_dir_ != active_dir_ || d >= kSteeringDeadbandDeg;
}

void FixedBinauralMvdr::resetStream() noexcept
{
  if (crossfading_)
  {
    active_azimuth_ = pending_azimuth_;
    active_dir_ = pending_dir_;
  }
  if (have_queued_)
  {
    active_azimuth_ = queued_azimuth_;
    active_dir_ = NearestAzimuthIndex(profile_, queued_azimuth_);
  }
  pending_azimuth_ = active_azimuth_;
  pending_dir_ = active_dir_;
  have_queued_ = false;
  crossfading_ = false;
  fade_cursor_ = 0;
  for (auto& stft : mic_stft_)
  {
    stft.reset();
  }
  left_stft_.reset();
  right_stft_.reset();
  std::fill(pz_smooth_.begin(), pz_smooth_.end(), 0.0F);
  std::fill(pr_smooth_.begin(), pr_smooth_.end(), 0.0F);
}

void FixedBinauralMvdr::OnMicHop(void* context, float* re, float* im, const std::size_t fft_size) noexcept
{
  auto* ctx = static_cast<MicHopContext*>(context);
  if (ctx == nullptr || ctx->self == nullptr)
  {
    return;
  }
  ctx->self->StoreMicSpectrum(ctx->channel, re, im, fft_size);
  if (ctx->channel + 1U == kM)
  {
    ctx->self->FormAndSynthesize();
  }
}

void FixedBinauralMvdr::StoreMicSpectrum(const std::size_t channel,
                                        const float* re,
                                        const float* im,
                                        const std::size_t fft_size) noexcept
{
  if (channel >= kM || re == nullptr || im == nullptr)
  {
    return;
  }
  std::memcpy(x_re_[channel].data(), re, fft_size * sizeof(float));
  std::memcpy(x_im_[channel].data(), im, fft_size * sizeof(float));
}

void FixedBinauralMvdr::ApplyHermitian(std::vector<float>& y_re, std::vector<float>& y_im) const noexcept
{
  y_im[0] = 0.0F;
  y_im[kFftSize / 2U] = 0.0F;
  for (std::size_t k = 1; k < kFftSize / 2U; ++k)
  {
    y_re[kFftSize - k] = y_re[k];
    y_im[kFftSize - k] = -y_im[k];
  }
}

void FixedBinauralMvdr::FormEarSpectra(const std::size_t dir_index,
                                      std::vector<float>& left_re,
                                      std::vector<float>& left_im,
                                      std::vector<float>& right_re,
                                      std::vector<float>& right_im,
                                      const bool update_smoother) noexcept
{
  const std::size_t bins = profile_.bin_count;
  const std::size_t mics = profile_.mic_count;
  const std::size_t left_i = profile_.left_ear_mic;
  const std::size_t right_i = profile_.right_ear_mic;
  const float gamma = profile_.reference_gain;
  const float rho_lo = DbToLin(mask_.eta_low_db);
  const float rho_hi = std::max(DbToLin(mask_.eta_high_db), rho_lo + 1.0e-6F);
  ++look_count_;

  for (std::size_t b = 0; b < bins; ++b)
  {
    const std::size_t base = ((dir_index * bins) + b) * mics * 2U;
    float zr = 0.0F;
    float zi = 0.0F;
    for (std::size_t ch = 0; ch < mics; ++ch)
    {
      const float wr = profile_.weights_ri[base + (ch * 2U)];
      const float wi = profile_.weights_ri[base + (ch * 2U) + 1U];
      const float xr = x_re_[ch][b];
      const float xi = x_im_[ch][b];
      zr += (wr * xr) + (wi * xi);
      zi += (wr * xi) - (wi * xr);
    }

    float pr = 0.0F;
    for (std::size_t ch = 0; ch < mics; ++ch)
    {
      const float dr = profile_.steering_ri[base + (ch * 2U)];
      const float di = profile_.steering_ri[base + (ch * 2U) + 1U];
      const float rr = x_re_[ch][b] - ((dr * zr) - (di * zi));
      const float ri = x_im_[ch][b] - ((dr * zi) + (di * zr));
      pr += (rr * rr) + (ri * ri);
    }
    pr /= static_cast<float>(mics);
    const float pz = (zr * zr) + (zi * zi);
    if (update_smoother)
    {
      pz_smooth_[b] = ((1.0F - mask_alpha_) * pz_smooth_[b]) + (mask_alpha_ * pz);
      pr_smooth_[b] = ((1.0F - mask_alpha_) * pr_smooth_[b]) + (mask_alpha_ * pr);
    }

    float g = 1.0F;
    if (mask_.constant_mask >= 0.0F)
    {
      g = std::clamp(mask_.constant_mask, 0.0F, 1.0F);
    }
    else if (mask_.enabled)
    {
      if (pz_smooth_[b] < mask_.quiet_power && pr_smooth_[b] < mask_.quiet_power)
      {
        g = 0.0F;
      }
      else
      {
        const float c = (b < profile_.dominance_scale.size()) ? profile_.dominance_scale[b] : 1.0F;
        const float rho = pz_smooth_[b] / ((c * pr_smooth_[b]) + 1.0e-12F);
        g = Smoothstep01((rho - rho_lo) / (rho_hi - rho_lo));
        if (profile_.valid[(dir_index * bins) + b] == 0)
        {
          g = 0.0F;
        }
      }
    }

    const float dlr = profile_.steering_ri[base + (left_i * 2U)];
    const float dli = profile_.steering_ri[base + (left_i * 2U) + 1U];
    const float drr = profile_.steering_ri[base + (right_i * 2U)];
    const float dri = profile_.steering_ri[base + (right_i * 2U) + 1U];
    const float blr = (dlr * zr) - (dli * zi);
    const float bli = (dlr * zi) + (dli * zr);
    const float brr = (drr * zr) - (dri * zi);
    const float bri = (drr * zi) + (dri * zr);
    const float xl_r = x_re_[left_i][b];
    const float xl_i = x_im_[left_i][b];
    const float xr_r = x_re_[right_i][b];
    const float xr_i = x_im_[right_i][b];
    const float og = 1.0F - g;
    left_re[b] = (g * blr) + (og * gamma * xl_r);
    left_im[b] = (g * bli) + (og * gamma * xl_i);
    right_re[b] = (g * brr) + (og * gamma * xr_r);
    right_im[b] = (g * bri) + (og * gamma * xr_i);
  }
}

void FixedBinauralMvdr::FormAndSynthesize() noexcept
{
  FormEarSpectra(active_dir_, left_y_re_, left_y_im_, right_y_re_, right_y_im_, true);
  if (crossfading_)
  {
    FormEarSpectra(pending_dir_, pending_left_re_, pending_left_im_, pending_right_re_,
                   pending_right_im_, false);
    const float alpha =
        static_cast<float>(fade_cursor_) / static_cast<float>(std::max<std::size_t>(1U, ramp_samples_));
    const float a = std::clamp(alpha, 0.0F, 1.0F);
    const float oa = 1.0F - a;
    for (std::size_t b = 0; b < profile_.bin_count; ++b)
    {
      left_y_re_[b] = (oa * left_y_re_[b]) + (a * pending_left_re_[b]);
      left_y_im_[b] = (oa * left_y_im_[b]) + (a * pending_left_im_[b]);
      right_y_re_[b] = (oa * right_y_re_[b]) + (a * pending_right_re_[b]);
      right_y_im_[b] = (oa * right_y_im_[b]) + (a * pending_right_im_[b]);
    }
  }
  ApplyHermitian(left_y_re_, left_y_im_);
  ApplyHermitian(right_y_re_, right_y_im_);
  left_stft_.overlapAddSpectrum(left_y_re_.data(), left_y_im_.data());
  right_stft_.overlapAddSpectrum(right_y_re_.data(), right_y_im_.data());
}

void FixedBinauralMvdr::processStereo(const std::span<const audio::MicFrame> input,
                                     const std::span<float> left_out,
                                     const std::span<float> right_out)
{
  if (!configured_)
  {
    throw std::runtime_error("fixed MVDR used before configure");
  }
  if (left_out.size() < input.size() || right_out.size() < input.size())
  {
    throw std::runtime_error("fixed MVDR stereo spans too small");
  }
  for (std::size_t i = 0; i < input.size(); ++i)
  {
    for (std::size_t ch = 0; ch < kM; ++ch)
    {
      mic_stft_[ch].feed(input[i][ch], &FixedBinauralMvdr::OnMicHop, &mic_ctx_[ch]);
    }
    left_out[i] = left_stft_.pop();
    right_out[i] = right_stft_.pop();
    if (crossfading_)
    {
      ++fade_cursor_;
      if (fade_cursor_ >= ramp_samples_)
      {
        active_azimuth_ = pending_azimuth_;
        active_dir_ = pending_dir_;
        crossfading_ = false;
        fade_cursor_ = 0;
        if (have_queued_)
        {
          const float next = queued_azimuth_;
          have_queued_ = false;
          pending_azimuth_ = next;
          pending_dir_ = NearestAzimuthIndex(profile_, next);
          const double d = std::fabs(spatial::NormalizeAzimuthDeg(
              static_cast<double>(pending_azimuth_ - active_azimuth_)));
          if (d >= kSteeringDeadbandDeg)
          {
            crossfading_ = true;
          }
        }
      }
    }
  }
}

std::size_t FixedBinauralMvdr::algorithmicDelaySamples() const noexcept
{
  return configured_ ? (kFftSize - 1U) : 0U;
}
}  // namespace sonitude::dsp
