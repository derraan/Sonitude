#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonitude::dsp
{
// Public transform contract shared by the host radix-2 backend and any future
// CMSIS-DSP adapter:
//   forward: unnormalized DFT (no 1/N)
//   inverse: DFT^{-1} scaled by 1/N
// so a forward+inverse round-trip is identity (within float32 error) without
// windows. Time domain is real; frequency domain is packed as separate real
// and imag arrays of length n.
class Radix2Fft
{
 public:
  // Allocates twiddles and bit-reversal indices. n must be a power of two.
  bool prepare(std::size_t n);
  void reset() noexcept;

  void forward(const float* time, float* re, float* im) const noexcept;
  // Mutates re/im in place (bit-reversal + butterflies), then writes time.
  void inverse(float* re, float* im, float* time) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  [[nodiscard]] std::size_t coefficientBytes() const noexcept;

 private:
  void BitReverse(float* re, float* im) const noexcept;
  void Butterfly(float* re, float* im, bool inverse) const noexcept;

  std::size_t n_ = 0;
  std::vector<float> wr_;
  std::vector<float> wi_;
  std::vector<std::uint32_t> bitrev_;
};
}  // namespace sonitude::dsp
