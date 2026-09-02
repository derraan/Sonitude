#include "dsp/streaming_stft.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sonitude::dsp
{
namespace
{
constexpr float kPi = 3.14159265358979323846F;

bool SupportedPair(const std::size_t fft, const std::size_t hop)
{
  return (fft == 128U && hop == 32U) || (fft == 256U && hop == 64U);
}

void PeriodicHann(std::vector<float>& window, const std::size_t n)
{
  window.assign(n, 0.0F);
  for (std::size_t i = 0; i < n; ++i)
  {
    window[i] =
        0.5F * (1.0F - std::cos(2.0F * kPi * static_cast<float>(i) / static_cast<float>(n)));
  }
}

float ColaOfWindowSquared(const std::vector<float>& window, const std::size_t hop)
{
  const std::size_t n = window.size();
  std::vector<float> acc(n, 0.0F);
  for (std::size_t offset = 0; offset < n; offset += hop)
  {
    for (std::size_t i = 0; i < n; ++i)
    {
      acc[(offset + i) % n] += window[i] * window[i];
    }
  }
  // Periodic Hann at 75% overlap is COLA-constant; use the centre value.
  return acc[n / 2U];
}
}  // namespace

bool StreamingStft::prepare(const double sample_rate,
                            const std::size_t maximum_block_frames,
                            const StreamingStftConfig& config)
{
  ready_ = false;
  if (!(sample_rate > 0.0) || maximum_block_frames == 0 || !SupportedPair(config.fft_size, config.hop_size))
  {
    return false;
  }
  if (!fft_.prepare(config.fft_size))
  {
    return false;
  }

  sample_rate_ = sample_rate;
  fft_size_ = config.fft_size;
  hop_size_ = config.hop_size;
  synthesize_ = config.synthesize;
  PeriodicHann(window_, fft_size_);
  const float cola = ColaOfWindowSquared(window_, hop_size_);
  if (!(cola > 1.0e-6F) || !std::isfinite(cola))
  {
    return false;
  }
  ola_scale_ = 1.0F / cola;

  analysis_ring_.assign(fft_size_, 0.0F);
  time_scratch_.assign(fft_size_, 0.0F);
  re_.assign(fft_size_, 0.0F);
  im_.assign(fft_size_, 0.0F);
  if (synthesize_)
  {
    ola_.assign(fft_size_, 0.0F);
    const std::size_t fifo = maximum_block_frames + fft_size_ + hop_size_;
    out_fifo_.assign(fifo, 0.0F);
  }
  else
  {
    ola_.clear();
    out_fifo_.clear();
  }

  reset();
  ready_ = true;
  return true;
}

void StreamingStft::reset() noexcept
{
  if (analysis_ring_.empty())
  {
    return;
  }
  std::fill(analysis_ring_.begin(), analysis_ring_.end(), 0.0F);
  std::fill(time_scratch_.begin(), time_scratch_.end(), 0.0F);
  std::fill(re_.begin(), re_.end(), 0.0F);
  std::fill(im_.begin(), im_.end(), 0.0F);
  std::fill(ola_.begin(), ola_.end(), 0.0F);
  std::fill(out_fifo_.begin(), out_fifo_.end(), 0.0F);
  ring_write_ = 0;
  hop_filled_ = 0;
  out_read_ = 0;
  out_write_ = 0;
  out_count_ = 0;
}

void StreamingStft::PushOutput(const float sample) noexcept
{
  if (out_count_ >= out_fifo_.size())
  {
    // Drop oldest to keep producing; should not happen with the sized fifo.
    out_read_ = (out_read_ + 1U) % out_fifo_.size();
    --out_count_;
  }
  out_fifo_[out_write_] = sample;
  out_write_ = (out_write_ + 1U) % out_fifo_.size();
  ++out_count_;
}

float StreamingStft::PopOutput() noexcept
{
  if (out_count_ == 0)
  {
    return 0.0F;
  }
  const float sample = out_fifo_[out_read_];
  out_read_ = (out_read_ + 1U) % out_fifo_.size();
  --out_count_;
  return sample;
}

void StreamingStft::ProcessHop(SpectralHopFn hop_fn, void* hop_context) noexcept
{
  for (std::size_t i = 0; i < fft_size_; ++i)
  {
    const std::size_t idx = (ring_write_ + i) % fft_size_;
    time_scratch_[i] = analysis_ring_[idx] * window_[i];
  }
  fft_.forward(time_scratch_.data(), re_.data(), im_.data());
  if (hop_fn != nullptr)
  {
    hop_fn(hop_context, re_.data(), im_.data(), fft_size_);
  }
  if (!synthesize_)
  {
    return;
  }
  fft_.inverse(re_.data(), im_.data(), time_scratch_.data());
  for (std::size_t i = 0; i < fft_size_; ++i)
  {
    ola_[i] += time_scratch_[i] * window_[i] * ola_scale_;
  }
  for (std::size_t i = 0; i < hop_size_; ++i)
  {
    PushOutput(ola_[i]);
  }
  std::memmove(ola_.data(), ola_.data() + hop_size_, (fft_size_ - hop_size_) * sizeof(float));
  std::fill(ola_.begin() + static_cast<std::ptrdiff_t>(fft_size_ - hop_size_), ola_.end(), 0.0F);
}

void StreamingStft::feed(const float sample, SpectralHopFn hop_fn, void* hop_context) noexcept
{
  if (!ready_ || analysis_ring_.empty())
  {
    return;
  }
  analysis_ring_[ring_write_] = sample;
  ring_write_ = (ring_write_ + 1U) % fft_size_;
  ++hop_filled_;
  if (hop_filled_ == hop_size_)
  {
    hop_filled_ = 0;
    ProcessHop(hop_fn, hop_context);
  }
}

void StreamingStft::overlapAddSpectrum(const float* re, const float* im) noexcept
{
  if (!ready_ || !synthesize_ || re == nullptr || im == nullptr || re_.empty())
  {
    return;
  }
  std::memcpy(re_.data(), re, fft_size_ * sizeof(float));
  std::memcpy(im_.data(), im, fft_size_ * sizeof(float));
  fft_.inverse(re_.data(), im_.data(), time_scratch_.data());
  for (std::size_t i = 0; i < fft_size_; ++i)
  {
    ola_[i] += time_scratch_[i] * window_[i] * ola_scale_;
  }
  for (std::size_t i = 0; i < hop_size_; ++i)
  {
    PushOutput(ola_[i]);
  }
  std::memmove(ola_.data(), ola_.data() + hop_size_, (fft_size_ - hop_size_) * sizeof(float));
  std::fill(ola_.begin() + static_cast<std::ptrdiff_t>(fft_size_ - hop_size_), ola_.end(), 0.0F);
}

float StreamingStft::pop() noexcept
{
  return PopOutput();
}

void StreamingStft::process(const std::span<const float> input,
                            const std::span<float> output,
                            SpectralHopFn hop_fn,
                            void* hop_context) noexcept
{
  const std::size_t n = std::min(input.size(), output.size());
  if (!ready_ || n == 0)
  {
    for (std::size_t i = 0; i < output.size(); ++i)
    {
      output[i] = 0.0F;
    }
    return;
  }
  for (std::size_t i = 0; i < n; ++i)
  {
    feed(input[i], hop_fn, hop_context);
    output[i] = synthesize_ ? PopOutput() : 0.0F;
  }
  for (std::size_t i = n; i < output.size(); ++i)
  {
    output[i] = 0.0F;
  }
}
}  // namespace sonitude::dsp
