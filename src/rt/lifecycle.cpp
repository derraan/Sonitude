#include "rt/lifecycle.hpp"

#include <csignal>

namespace sonitude::rt
{
namespace
{
// Only ever set once, by the supervisor, before signal handlers are installed.
std::atomic<Lifecycle*> g_signal_target{nullptr};

void HandleStopSignal(int)
{
  Lifecycle* target = g_signal_target.load(std::memory_order_acquire);
  if (target != nullptr)
  {
    target->requestStop(StopReason::Signal);
  }
}
}  // namespace

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
  (void)reason_.compare_exchange_strong(expected,
                                        static_cast<std::uint8_t>(reason),
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
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

bool InstallSignalHandlers(Lifecycle& lifecycle) noexcept
{
  g_signal_target.store(&lifecycle, std::memory_order_release);
  return std::signal(SIGINT, HandleStopSignal) != SIG_ERR &&
         std::signal(SIGTERM, HandleStopSignal) != SIG_ERR;
}
}  // namespace sonitude::rt
