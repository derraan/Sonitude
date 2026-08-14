#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__linux__)
#include <signal.h>
#endif

#include "rt/wake_event.hpp"

namespace sonitude::rt
{
enum class StopReason : std::uint8_t
{
  None = 0,
  Signal = 1,
  StartupFailure = 2,
  CaptureFailure = 3,
  PlaybackFailure = 4,
  ControlFailure = 5,
  CaptureStreamEnded = 6,
};

const char* StopReasonName(StopReason reason) noexcept;

// The single shutdown authority for the process.
//
// Exactly one Lifecycle exists, it is owned by the supervisor (main), and it
// outlives every thread that observes it. Nothing else in the runtime is
// allowed to invent its own stop flag: there is no separate g_running, no
// per-thread stop_token, and no condition-variable "please exit" protocol.
//
// Any thread (and the SIGINT/SIGTERM handler) may call requestStop(). The first
// caller's reason is the one that is reported; later calls only re-signal the
// wake event. Stop is sticky: once requested it is never cleared, so a thread
// that checks late still observes it, and the wake event stays readable so a
// late poll() returns immediately rather than waiting out its timeout.
class Lifecycle
{
  static_assert(std::atomic<bool>::is_always_lock_free,
                "the stop flag is set from a signal handler and must be lock-free");
  static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                "the stop reason is set from a signal handler and must be lock-free");

public:
  Lifecycle() = default;
  Lifecycle(const Lifecycle&) = delete;
  Lifecycle& operator=(const Lifecycle&) = delete;

  // Idempotent. Async-signal-safe on Linux.
  void requestStop(StopReason reason) noexcept;

  bool stopRequested() const noexcept
  {
    return stop_.load(std::memory_order_acquire);
  }
  StopReason reason() const noexcept
  {
    return static_cast<StopReason>(reason_.load(std::memory_order_acquire));
  }

  // Slow-path threads use this instead of sleeping on a timer: it returns as
  // soon as shutdown is requested, and otherwise after the requested period.
  // Returns true when shutdown has been requested.
  bool waitForStopOr(std::chrono::milliseconds period) noexcept;

  // Pollable descriptor that becomes readable on shutdown, for threads that
  // must block on shutdown together with another descriptor (e.g. ALSA).
  int wakeFd() const noexcept
  {
    return wake_.fd();
  }
  WakeEvent& wakeEvent() noexcept
  {
    return wake_;
  }

private:
  std::atomic<bool> stop_{false};
  std::atomic<std::uint8_t> reason_{static_cast<std::uint8_t>(StopReason::None)};
  WakeEvent wake_;
};

static_assert(std::atomic<Lifecycle*>::is_always_lock_free,
              "the signal target pointer must always be lock-free");

// Owns the process SIGINT/SIGTERM installation. Declare this immediately after
// its Lifecycle so this object is destroyed first. Destruction blocks the
// handled signals, clears the target, and restores both previous handlers
// before the Lifecycle can be destroyed.
class SignalHandlerInstallation
{
public:
  explicit SignalHandlerInstallation(Lifecycle& lifecycle) noexcept;
  ~SignalHandlerInstallation();

  SignalHandlerInstallation(const SignalHandlerInstallation&) = delete;
  SignalHandlerInstallation& operator=(const SignalHandlerInstallation&) = delete;
  SignalHandlerInstallation(SignalHandlerInstallation&&) = delete;
  SignalHandlerInstallation& operator=(SignalHandlerInstallation&&) = delete;

  bool installed() const noexcept
  {
    return installed_;
  }

private:
#if defined(__linux__)
  struct sigaction previous_int_
  {
  };
  struct sigaction previous_term_
  {
  };
#endif
  bool installed_ = false;
};
} // namespace sonitude::rt
