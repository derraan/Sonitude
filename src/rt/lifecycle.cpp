#include "rt/lifecycle.hpp"

#if defined(__linux__)
#include <pthread.h>
#endif

namespace sonitude::rt
{
namespace
{
std::atomic<Lifecycle*> g_signal_target{nullptr};

void HandleStopSignal(int) noexcept
{
  Lifecycle* target = g_signal_target.load(std::memory_order_acquire);
  if (target != nullptr)
  {
    target->requestStop(StopReason::Signal);
  }
}

#if defined(__linux__)
sigset_t StopSignalSet() noexcept
{
  sigset_t signals;
  (void)::sigemptyset(&signals);
  (void)::sigaddset(&signals, SIGINT);
  (void)::sigaddset(&signals, SIGTERM);
  return signals;
}
#endif
} // namespace

const char* StopReasonName(const StopReason reason) noexcept
{
  switch (reason)
  {
  case StopReason::None:
    return "none";
  case StopReason::Signal:
    return "signal";
  case StopReason::StartupFailure:
    return "startup-failure";
  case StopReason::CaptureFailure:
    return "capture-failure";
  case StopReason::PlaybackFailure:
    return "playback-failure";
  case StopReason::ControlFailure:
    return "control-failure";
  case StopReason::CaptureStreamEnded:
    return "capture-stream-ended";
  }
  return "unknown";
}

void Lifecycle::requestStop(const StopReason reason) noexcept
{
  std::uint8_t expected = static_cast<std::uint8_t>(StopReason::None);
  // First reason wins; a losing racer still falls through to signal the event.
  (void)reason_.compare_exchange_strong(expected, static_cast<std::uint8_t>(reason),
                                        std::memory_order_acq_rel, std::memory_order_acquire);
  stop_.store(true, std::memory_order_release);
  wake_.signal();
}

bool Lifecycle::waitForStopOr(const std::chrono::milliseconds period) noexcept
{
  if (stopRequested())
  {
    return true;
  }
  (void)wake_.waitFor(period);
  return stopRequested();
}

SignalHandlerInstallation::SignalHandlerInstallation(Lifecycle& lifecycle) noexcept
{
#if defined(__linux__)
  const sigset_t signals = StopSignalSet();
  sigset_t previous_mask;
  if (::pthread_sigmask(SIG_BLOCK, &signals, &previous_mask) != 0)
  {
    return;
  }

  Lifecycle* expected = nullptr;
  if (!g_signal_target.compare_exchange_strong(expected, &lifecycle, std::memory_order_release,
                                               std::memory_order_relaxed))
  {
    (void)::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    return;
  }

  struct sigaction action
  {
  };
  action.sa_handler = HandleStopSignal;
  (void)::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;

  const bool int_installed = ::sigaction(SIGINT, &action, &previous_int_) == 0;
  const bool term_installed = int_installed && ::sigaction(SIGTERM, &action, &previous_term_) == 0;
  if (!term_installed)
  {
    if (int_installed)
    {
      (void)::sigaction(SIGINT, &previous_int_, nullptr);
    }
    g_signal_target.store(nullptr, std::memory_order_release);
  }
  else
  {
    installed_ = true;
  }
  (void)::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
#else
  (void)lifecycle;
#endif
}

SignalHandlerInstallation::~SignalHandlerInstallation()
{
#if defined(__linux__)
  if (!installed_)
  {
    return;
  }

  // Runtime threads are joined before this owner is destroyed. Blocking both
  // signals on the remaining supervisor closes the final teardown window:
  // after the target is cleared, no invocation can acquire the Lifecycle.
  const sigset_t signals = StopSignalSet();
  sigset_t previous_mask;
  const bool mask_changed = ::pthread_sigmask(SIG_BLOCK, &signals, &previous_mask) == 0;

  g_signal_target.store(nullptr, std::memory_order_release);
  (void)::sigaction(SIGTERM, &previous_term_, nullptr);
  (void)::sigaction(SIGINT, &previous_int_, nullptr);
  installed_ = false;

  if (mask_changed)
  {
    (void)::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
  }
#endif
}
} // namespace sonitude::rt
