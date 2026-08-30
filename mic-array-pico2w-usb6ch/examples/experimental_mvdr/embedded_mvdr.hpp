#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "arm_math.h"

namespace sonitude::embedded {

constexpr std::size_t kMicrophones = 6;
constexpr std::size_t kFftSize = 128;
constexpr std::size_t kHopSize = 32;  // 75% overlap; matches the host PR #34 backend.
constexpr std::size_t kBins = kFftSize / 2 + 1;
constexpr std::size_t kBlockFrames = 64;

struct Complex {
  float re;
  float im;
};

using Spectrum = std::array<std::array<Complex, kBins>, kMicrophones>;
using WeightSet = std::array<std::array<Complex, kMicrophones>, kBins>;

// Payloads live in shared SRAM; the hardware FIFO carries only the published
// generation number. A release/acquire generation hand-off prevents readers
// from observing a partially written bank.
template <typename T>
struct DoubleBufferMailbox {
  alignas(32) std::array<T, 2> bank{};
  std::atomic<std::uint32_t> generation{0};
  mutable std::atomic<std::uint32_t> consumed_generation{0};

  bool publish(const T& value) noexcept {
    const auto current = generation.load(std::memory_order_relaxed);
    const auto consumed = consumed_generation.load(std::memory_order_acquire);
    if (current - consumed >= 2U) return false;  // Consumer is behind; drop this update.
    const auto next = current + 1U;
    bank[next & 1U] = value;
    generation.store(next, std::memory_order_release);
    return true;
  }

  bool readIfNew(std::uint32_t& seen, T& value) const noexcept {
    const auto available = generation.load(std::memory_order_acquire);
    if (available == seen) return false;
    value = bank[available & 1U];
    seen = available;
    consumed_generation.store(available, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::uint32_t publishedGeneration() const noexcept {
    return generation.load(std::memory_order_acquire);
  }
};

struct FastOvdConfig {
  bool enabled = false;
  std::uint8_t front_channel = 0;
  std::uint8_t rear_channel = 0;
  float energy_ratio_threshold = 0.0F;  // Must come from headset measurements.
  float active_gain = 1.0F;             // Do not assume -12 dB without a listening test.
  float attack_step = 1.0F;
  float release_step = 1.0F;
};

struct SlowPathConfig {
  bool adaptation_enabled = false;
  float covariance_alpha = 0.0F;       // 0 < alpha <= 1, derived from a time constant.
  float diagonal_load_relative = 0.0F; // Fraction of mean bin power, not an absolute load.
  float minimum_bin_power = 1.0e-12F;
  std::uint32_t weight_update_interval_hops = 32; // Never invert every audio hop by default.
};

struct RuntimeCounters {
  std::uint32_t processed_hops = 0;
  std::uint32_t singular_bins = 0;
  std::uint32_t invalid_samples = 0;
};

class FastPath {
 public:
  bool init(DoubleBufferMailbox<WeightSet>* weights,
            DoubleBufferMailbox<Spectrum>* snapshots,
            FastOvdConfig ovd) noexcept;
  void processBlock(const float input[kMicrophones][kBlockFrames],
                    float output[kBlockFrames]) noexcept;
  void releaseOwnVoiceGain() noexcept { ovd_latched_ = false; }
  [[nodiscard]] bool ownVoiceLatched() const noexcept { return ovd_latched_; }
  [[nodiscard]] const RuntimeCounters& counters() const noexcept { return counters_; }

 private:
  void processHop() noexcept;
  void unpackSpectrum(const float packed[kFftSize], Complex out[kBins]) noexcept;
  void packSpectrum(const Complex in[kBins], float packed[kFftSize]) noexcept;

  DoubleBufferMailbox<WeightSet>* weights_mailbox_ = nullptr;
  DoubleBufferMailbox<Spectrum>* snapshots_mailbox_ = nullptr;
  arm_rfft_fast_instance_f32 fft_{};
  FastOvdConfig ovd_{};
  WeightSet weights_{};
  std::uint32_t weight_generation_ = 0;
  std::array<std::array<float, kFftSize>, kMicrophones> ring_{};
  std::array<std::array<float, kFftSize>, kMicrophones> time_{};
  std::array<std::array<float, kFftSize>, kMicrophones> packed_{};
  Spectrum spectra_{};
  std::array<float, kFftSize> window_{};
  std::array<float, kFftSize> output_packed_{};
  std::array<float, kFftSize> inverse_{};
  std::array<float, kFftSize> ola_{};
  std::array<float, kFftSize + kHopSize> output_fifo_{};
  std::size_t ring_write_ = 0;
  std::size_t hop_filled_ = 0;
  std::size_t out_read_ = 0;
  std::size_t out_write_ = 0;
  std::size_t out_count_ = 0;
  float ovd_gain_ = 1.0F;
  bool ovd_latched_ = false;
  RuntimeCounters counters_{};
};

class SlowPath {
 public:
  bool init(DoubleBufferMailbox<Spectrum>* snapshots,
            DoubleBufferMailbox<WeightSet>* weights,
            SlowPathConfig config) noexcept;
  void setSteering(const WeightSet& steering_vectors) noexcept;
  bool poll() noexcept;
  [[nodiscard]] const RuntimeCounters& counters() const noexcept { return counters_; }

 private:
  bool updateWeights() noexcept;

  DoubleBufferMailbox<Spectrum>* snapshots_mailbox_ = nullptr;
  DoubleBufferMailbox<WeightSet>* weights_mailbox_ = nullptr;
  SlowPathConfig config_{};
  Spectrum snapshot_{};
  WeightSet steering_{};
  WeightSet weights_{};
  std::array<std::array<std::array<Complex, kMicrophones>, kMicrophones>, kBins> covariance_{};
  std::uint32_t snapshot_generation_ = 0;
  std::uint32_t hops_since_weight_update_ = 0;
  RuntimeCounters counters_{};
};

}  // namespace sonitude::embedded
