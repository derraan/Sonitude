#pragma once

#include <atomic>
#include <cstdint>

namespace sonitude::rt
{
struct TelemetryCounters
{
  std::atomic<std::uint64_t> capture_frames{0};
  std::atomic<std::uint64_t> playback_frames{0};
  std::atomic<std::uint64_t> capture_xruns{0};
  std::atomic<std::uint64_t> playback_xruns{0};
  std::atomic<std::uint64_t> playback_write_failures{0};
  std::atomic<std::uint64_t> ring_overruns{0};
  std::atomic<std::uint64_t> ring_underruns{0};
  std::atomic<std::int64_t> asrc_ratio_ppm{0};
  std::atomic<std::int64_t> ring_occupancy_frames{0};
  std::atomic<std::int64_t> suppressor_gain_milli{1000};
  std::atomic<std::int64_t> steering_confidence_milli{0};
  std::atomic<std::int64_t> speech_probability_milli{0};
  std::atomic<std::uint8_t> control_state{0};
};
}  // namespace sonitude::rt
