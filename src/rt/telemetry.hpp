#pragma once

#include <atomic>
#include <cstdint>

namespace sonitude::rt
{
// Counters written by realtime threads and read by the telemetry thread.
//
// Every member here is telemetry-only: nothing in the runtime makes a control
// decision from these values, and no other memory is ordered against them.
// memory_order_relaxed is therefore correct and intentional, not a shortcut --
// there is no happens-before edge to establish, only eventual visibility.
//
// The realtime threads must never format, log, or aggregate these; they only
// store. All reading, formatting, and printing happens on the telemetry thread.
struct TelemetryCounters
{
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "realtime threads update these counters and must not block on them");
  static_assert(std::atomic<std::int64_t>::is_always_lock_free,
                "realtime threads update these counters and must not block on them");

  // Throughput.
  std::atomic<std::uint64_t> capture_frames{0};
  std::atomic<std::uint64_t> playback_frames{0};

  // Device faults.
  std::atomic<std::uint64_t> capture_xruns{0};
  std::atomic<std::uint64_t> playback_xruns{0};
  std::atomic<std::uint64_t> playback_write_failures{0};
  std::atomic<std::uint64_t> capture_wait_timeouts{0};
  std::atomic<std::uint64_t> capture_wait_errors{0};
  std::atomic<std::uint64_t> capture_overflow_refusals{0};

  // Block ownership pipeline.
  std::atomic<std::uint64_t> ring_overruns{0};       // producer had no free slot
  std::atomic<std::uint64_t> ring_underruns{0};      // playback had no ready block
  std::atomic<std::uint64_t> block_commit_failures{0};

  // ASRC.
  std::atomic<std::int64_t> asrc_ratio_ppm{0};
  std::atomic<std::int64_t> ring_occupancy_frames{0};

  // Loop timing, in microseconds, sampled by the owning realtime thread.
  std::atomic<std::uint64_t> capture_period_max_us{0};
  std::atomic<std::uint64_t> capture_period_last_us{0};
  std::atomic<std::uint64_t> capture_work_max_us{0};
  std::atomic<std::uint64_t> capture_deadline_misses{0};
  std::atomic<std::uint64_t> playback_write_max_us{0};
  std::atomic<std::uint64_t> playback_write_last_us{0};

  // Control freshness.
  std::atomic<std::uint64_t> control_snapshot_age_us{0};
  std::atomic<std::uint64_t> control_updates_applied{0};
  std::atomic<std::uint64_t> control_ticks{0};

  // DSP observability.
  std::atomic<std::int64_t> suppressor_gain_milli{1000};
  std::atomic<std::int64_t> steering_confidence_milli{0};
  std::atomic<std::int64_t> speech_probability_milli{0};
  std::atomic<std::uint8_t> control_state{0};
};

// Stores `value` into `target` if it is larger. Realtime-safe: a relaxed
// compare-exchange loop on a lock-free atomic, bounded by the number of
// concurrent writers, and each counter here has exactly one writer thread.
inline void StoreMaxRelaxed(std::atomic<std::uint64_t>& target, const std::uint64_t value) noexcept
{
  std::uint64_t current = target.load(std::memory_order_relaxed);
  while (value > current &&
         !target.compare_exchange_weak(
             current, value, std::memory_order_relaxed, std::memory_order_relaxed))
  {
  }
}
}  // namespace sonitude::rt
