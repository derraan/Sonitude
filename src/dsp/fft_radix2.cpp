#include "dsp/fft_backend.hpp"

#include <cmath>
#include <cstddef>

namespace sonitude::dsp
{
namespace
{
constexpr float kPi = 3.14159265358979323846F;

bool IsPowerOfTwo(const std::size_t n)
{
  return n >= 2 && (n & (n - 1U)) == 0;
}
}  // namespace

bool Radix2Fft::prepare(const std::size_t n)
{
  if (!IsPowerOfTwo(n) || n > 4096U)
  {
    n_ = 0;
    wr_.clear();
    wi_.clear();
    bitrev_.clear();
    return false;
  }

  n_ = n;
  wr_.assign(n / 2U, 0.0F);
  wi_.assign(n / 2U, 0.0F);
  bitrev_.assign(n, 0);
  for (std::size_t k = 0; k < n / 2U; ++k)
  {
    const float angle = -2.0F * kPi * static_cast<float>(k) / static_cast<float>(n);
    wr_[k] = std::cos(angle);
    wi_[k] = std::sin(angle);
  }

  std::size_t bits = 0;
  for (std::size_t m = n; m > 1U; m >>= 1U)
  {
    ++bits;
  }
  for (std::size_t i = 0; i < n; ++i)
  {
    std::uint32_t rev = 0;
    std::size_t x = i;
    for (std::size_t b = 0; b < bits; ++b)
    {
      rev = (rev << 1U) | static_cast<std::uint32_t>(x & 1U);
      x >>= 1U;
    }
    bitrev_[i] = rev;
  }
  return true;
}

void Radix2Fft::reset() noexcept {}

void Radix2Fft::BitReverse(float* const re, float* const im) const noexcept
{
  for (std::size_t i = 0; i < n_; ++i)
  {
    const std::size_t j = bitrev_[i];
    if (j > i)
    {
      const float tr = re[i];
      re[i] = re[j];
      re[j] = tr;
      const float ti = im[i];
      im[i] = im[j];
      im[j] = ti;
    }
  }
}

void Radix2Fft::Butterfly(float* const re, float* const im, const bool inverse) const noexcept
{
  for (std::size_t span = 2; span <= n_; span <<= 1U)
  {
    const std::size_t half = span / 2U;
    const std::size_t twiddle_step = n_ / span;
    for (std::size_t start = 0; start < n_; start += span)
    {
      std::size_t tw = 0;
      for (std::size_t k = 0; k < half; ++k)
      {
        const float wr = wr_[tw];
        const float wi = inverse ? -wi_[tw] : wi_[tw];
        const std::size_t i = start + k;
        const std::size_t j = i + half;
        const float tr = (wr * re[j]) - (wi * im[j]);
        const float ti = (wr * im[j]) + (wi * re[j]);
        re[j] = re[i] - tr;
        im[j] = im[i] - ti;
        re[i] += tr;
        im[i] += ti;
        tw += twiddle_step;
      }
    }
  }
}

void Radix2Fft::forward(const float* const time, float* const re, float* const im) const noexcept
{
  if (n_ == 0)
  {
    return;
  }
  for (std::size_t i = 0; i < n_; ++i)
  {
    re[i] = time[i];
    im[i] = 0.0F;
  }
  BitReverse(re, im);
  Butterfly(re, im, false);
}

void Radix2Fft::inverse(float* const re, float* const im, float* const time) const noexcept
{
  if (n_ == 0)
  {
    return;
  }
  BitReverse(re, im);
  Butterfly(re, im, true);
  const float scale = 1.0F / static_cast<float>(n_);
  for (std::size_t i = 0; i < n_; ++i)
  {
    time[i] = re[i] * scale;
  }
}

std::size_t Radix2Fft::coefficientBytes() const noexcept
{
  return (wr_.size() + wi_.size()) * sizeof(float) + (bitrev_.size() * sizeof(std::uint32_t));
}
}  // namespace sonitude::dsp
