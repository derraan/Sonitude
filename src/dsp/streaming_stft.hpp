#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dsp/fft_backend.hpp"

namespace sonitude::dsp
{
// Optional per-hop spectral callback. Called after the forward FFT and before
// the inverse. re/im are length fft_size in normal bin order. Must be
// allocation-free and non-throwing.
using SpectralHopFn = void (*)(void* context, float* re, float* im, std::size_t fft_size) noexcept;

struct StreamingStftConfig
{
  std::size_t fft_size = 128;
  std::size_t hop_size = 32;
  bool synthesize = true;
};

// Allocation-free after prepare. Streaming short-STFT with periodic Hann
// analysis/synthesis and explicit overlap-add normalization. Supported
// (fft, hop) pairs: (128, 32) and (256, 64). 512/128 is rejected.
class StreamingStft
{
 public:
  bool prepare(double sample_rate,
               std::size_t maximum_block_frames,
               const StreamingStftConfig& config);
  void reset() noexcept;
  void process(std::span<const float> input,
               std::span<float> output,
               SpectralHopFn hop_fn = nullptr,
               void* hop_context = nullptr) noexcept;
  // One sample. Hop callback runs when a hop is complete. Synthesis-only
  // instances should call pop() after each feed; analysis-only instances
  // ignore pop().
  void feed(float sample, SpectralHopFn hop_fn = nullptr, void* hop_context = nullptr) noexcept;
  // Inverse + OLA of an external spectrum. Call once per analysis hop on a
  // prepare(..., {.synthesize = true}) instance. Does not consume analysis PCM.
  void overlapAddSpectrum(const float* re, const float* im) noexcept;
  [[nodiscard]] float pop() noexcept;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] std::size_t fftSize() const noexcept { return fft_size_; }
  [[nodiscard]] std::size_t hopSize() const noexcept { return hop_size_; }
  [[nodiscard]] double sampleRate() const noexcept { return sample_rate_; }
  // Classic OLA look-ahead N-H. Not a substitute for the impulse first-arrival
  // measurement used as algorithmic delay.
  [[nodiscard]] std::size_t calculatedLookaheadSamples() const noexcept;
  [[nodiscard]] std::size_t persistentBytes() const noexcept;
  [[nodiscard]] float olaScale() const noexcept { return ola_scale_; }

 private:
  void ProcessHop(SpectralHopFn hop_fn, void* hop_context) noexcept;
  void PushOutput(float sample) noexcept;
  float PopOutput() noexcept;

  bool ready_ = false;
  bool synthesize_ = true;
  double sample_rate_ = 0.0;
  std::size_t fft_size_ = 0;
  std::size_t hop_size_ = 0;
  float ola_scale_ = 1.0F;

  Radix2Fft fft_{};
  std::vector<float> window_{};
  std::vector<float> analysis_ring_{};
  std::size_t ring_write_ = 0;
  std::size_t hop_filled_ = 0;
  std::vector<float> time_scratch_{};
  std::vector<float> re_{};
  std::vector<float> im_{};
  std::vector<float> ola_{};
  std::vector<float> out_fifo_{};
  std::size_t out_read_ = 0;
  std::size_t out_write_ = 0;
  std::size_t out_count_ = 0;
};
}  // namespace sonitude::dsp
