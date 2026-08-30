#include <array>
#include <cstddef>
#include <cstdint>

#include "embedded_mvdr.hpp"
#include "hardware/clocks.h"
#include "pico/clocks_audio.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/stdlib.h"

extern "C" {
#include "pico/microphone_array_i2s.h"
}

namespace se = sonitude::embedded;

namespace {
constexpr std::uint32_t kSampleRate = 48000;
constexpr unsigned int kSckPin = 0;
constexpr unsigned int kSd0Pin = 2;
constexpr std::size_t kCaptureRingBytes = 49152;
constexpr std::uint32_t kSnapshotToken = 0x53000000U;
constexpr std::uint32_t kWeightToken = 0x57000000U;
constexpr std::uint32_t kOvdReleaseToken = 0x4f000001U;

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "The mailbox hand-off requires lock-free 32-bit atomics");

alignas(32) se::DoubleBufferMailbox<se::WeightSet> g_weights;
alignas(32) se::DoubleBufferMailbox<se::Spectrum> g_snapshots;
alignas(32) se::FastPath g_fast_path;
alignas(32) se::SlowPath g_slow_path;
alignas(32) i2s_audio_sample g_capture[se::kBlockFrames];
alignas(32) float g_input[se::kMicrophones][se::kBlockFrames];
alignas(32) float g_output[se::kBlockFrames];

// The external ANC codec is not present in this repository. A board integration
// must provide this non-blocking function and consume exactly one 64-frame block.
extern "C" __attribute__((weak)) bool sonitude_anc_write_block(const float*, std::size_t) {
  return false;
}

void decode_capture_block() noexcept {
  constexpr float kInt32ToFloat = 1.0F / 2147483648.0F;
  for (std::size_t i = 0; i < se::kBlockFrames; ++i) {
    const auto sample = decode_sample(&g_capture[i]);
    g_input[0][i] = static_cast<float>(static_cast<std::int32_t>(sample.sample_l[0])) * kInt32ToFloat;
    g_input[1][i] = static_cast<float>(static_cast<std::int32_t>(sample.sample_r[0])) * kInt32ToFloat;
    g_input[2][i] = static_cast<float>(static_cast<std::int32_t>(sample.sample_l[1])) * kInt32ToFloat;
    g_input[3][i] = static_cast<float>(static_cast<std::int32_t>(sample.sample_r[1])) * kInt32ToFloat;
    g_input[4][i] = static_cast<float>(static_cast<std::int32_t>(sample.sample_l[2])) * kInt32ToFloat;
    g_input[5][i] = static_cast<float>(static_cast<std::int32_t>(sample.sample_r[2])) * kInt32ToFloat;
  }
}

// Core 1 owns capture and the hard real-time DSP loop. create_microphone_array_i2s()
// configures PIO, chained DMA ping-pong channels, and the DMA IRQ handlers. The
// IRQ only copies a completed DMA half into the driver's SPSC ring; FFT and matrix
// work never execute in interrupt context.
void __not_in_flash_func(core1_entry)() {
  auto* capture = create_microphone_array_i2s(
      0, kSckPin, kSd0Pin, 32, static_cast<std::int32_t>(kCaptureRingBytes), kSampleRate);
  if (capture == nullptr) panic("I2S capture initialization failed");

  se::FastOvdConfig ovd{};  // Disabled until channel pair and threshold are measured.
  if (!g_fast_path.init(&g_weights, &g_snapshots, ovd)) panic("Fast DSP initialization failed");

  std::uint32_t last_snapshot_token = 0;
  while (true) {
    while (multicore_fifo_rvalid()) {
      const auto token = multicore_fifo_pop_blocking();
      if (token == kOvdReleaseToken) g_fast_path.releaseOwnVoiceGain();
      // Weight tokens are hints; FastPath validates the mailbox generation itself.
    }

    const auto wanted = sizeof(g_capture);
    const int received = microphone_array_i2s_read_stream(capture, g_capture, wanted);
    if (received != static_cast<int>(wanted)) continue;
    decode_capture_block();
    g_fast_path.processBlock(g_input, g_output);
    (void)sonitude_anc_write_block(g_output, se::kBlockFrames);

    const auto generation = g_snapshots.publishedGeneration();
    if (generation != last_snapshot_token) {
      if (multicore_fifo_push_timeout_us(kSnapshotToken | (generation & 0x00ffffffU), 0))
        last_snapshot_token = generation;
    }
  }
}
}  // namespace

int main() {
  // Must precede PIO setup. The USB clock remains independent.
  set_sys_clock_khz(AUDIO_F_SYS_KHZ, true);
  stdio_init_all();

  se::SlowPathConfig slow{};
  // Adaptation is deliberately off: alpha, diagonal loading, and steering vectors
  // are measurement inputs, not universal constants.
  if (!g_slow_path.init(&g_snapshots, &g_weights, slow)) panic("Slow DSP initialization failed");
  multicore_launch_core1(core1_entry);

  std::uint32_t last_weight_token = g_weights.publishedGeneration();
  while (true) {
    bool snapshot_hint = false;
    while (multicore_fifo_rvalid()) {
      const auto token = multicore_fifo_pop_blocking();
      snapshot_hint |= (token & 0xff000000U) == kSnapshotToken;
    }
    if (snapshot_hint) (void)g_slow_path.poll();

    const auto generation = g_weights.publishedGeneration();
    if (generation != last_weight_token) {
      if (multicore_fifo_push_timeout_us(kWeightToken | (generation & 0x00ffffffU), 0))
        last_weight_token = generation;
    }
    tight_loop_contents();
  }
}
