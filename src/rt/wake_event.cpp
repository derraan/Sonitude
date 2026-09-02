#include "rt/wake_event.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#if defined(__linux__)
#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace sonitude::rt
{
namespace
{
#if defined(__linux__)
// poll() treats a negative timeout as "block forever". No caller wants that, so
// a nonsensical duration becomes a zero-length poll rather than a silent hang.
int PollTimeoutMs(const std::chrono::milliseconds timeout) noexcept
{
  using Rep = std::chrono::milliseconds::rep;
  const Rep count = timeout.count();
  if (count <= 0)
  {
    return 0;
  }
  if (count > static_cast<Rep>(std::numeric_limits<int>::max()))
  {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(count);
}
#endif
} // namespace

#if defined(__linux__)
WakeEvent::WakeEvent()
{
  fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd_ < 0)
  {
    throw std::runtime_error("eventfd creation failed for WakeEvent");
  }
}

WakeEvent::~WakeEvent()
{
  if (fd_ >= 0)
  {
    ::close(fd_);
  }
}

void WakeEvent::signal() noexcept
{
  const std::uint64_t token = 1;
  // EAGAIN here means the 64-bit counter would overflow, which can only happen
  // if ~2^64 wakeups went unconsumed. A wakeup is already pending in that case,
  // so dropping this one cannot lose an edge.
  const ssize_t written = ::write(fd_, &token, sizeof(token));
  (void)written;
}

bool WakeEvent::consume() noexcept
{
  std::uint64_t token = 0;
  return ::read(fd_, &token, sizeof(token)) == static_cast<ssize_t>(sizeof(token));
}

bool WakeEvent::waitFor(const std::chrono::milliseconds timeout) noexcept
{
  pollfd pfd{};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  const int poll_timeout_ms = PollTimeoutMs(timeout);
  for (;;)
  {
    const int rc = ::poll(&pfd, 1, poll_timeout_ms);
    if (rc < 0 && errno == EINTR)
    {
      continue;
    }
    return rc > 0 && (pfd.revents & POLLIN) != 0;
  }
}

WaitOutcome WaitAnyOf(const WakeEvent& first, const WakeEvent& second,
                      const std::chrono::milliseconds timeout) noexcept
{
  pollfd pfds[2]{};
  pfds[0].fd = first.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = second.fd();
  pfds[1].events = POLLIN;
  const int poll_timeout_ms = PollTimeoutMs(timeout);
  for (;;)
  {
    const int rc = ::poll(pfds, 2, poll_timeout_ms);
    if (rc < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      return WaitOutcome::Error;
    }
    if (rc == 0)
    {
      return WaitOutcome::Timeout;
    }
    if ((pfds[1].revents & POLLIN) != 0)
    {
      return WaitOutcome::Second;
    }
    if ((pfds[0].revents & POLLIN) != 0)
    {
      return WaitOutcome::First;
    }
    return WaitOutcome::Error;
  }
}
#else
WakeEvent::WakeEvent() = default;
WakeEvent::~WakeEvent() = default;

void WakeEvent::signal() noexcept
{
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    signalled_ = true;
  }
  cv_.notify_all();
}

bool WakeEvent::consume() noexcept
{
  const std::lock_guard<std::mutex> lock(mutex_);
  const bool was = signalled_;
  signalled_ = false;
  return was;
}

bool WakeEvent::waitFor(const std::chrono::milliseconds timeout) noexcept
{
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, timeout, [this] { return signalled_; });
}

// Host-tooling fallback only: no realtime path reaches this build. Without a
// pollable descriptor there is no way to block on two events in one call, so
// this samples both in short slices. That is polling, and it is acceptable here
// precisely because nothing on this platform has an audio deadline; it must not
// be reused on Linux, where the eventfd path above blocks on both at once.
WaitOutcome WaitAnyOf(const WakeEvent& first, const WakeEvent& second,
                      const std::chrono::milliseconds timeout) noexcept
{
  constexpr std::chrono::milliseconds kSlice{1};
  constexpr std::chrono::milliseconds kNone{0};
  WakeEvent& mutable_first = const_cast<WakeEvent&>(first);
  WakeEvent& mutable_second = const_cast<WakeEvent&>(second);
  std::chrono::milliseconds remaining = std::max(timeout, kNone);
  for (;;)
  {
    // Shutdown takes precedence over more work, matching the Linux behaviour.
    if (mutable_second.waitFor(kNone))
    {
      return WaitOutcome::Second;
    }
    if (mutable_first.waitFor(kNone))
    {
      return WaitOutcome::First;
    }
    if (remaining <= kNone)
    {
      return WaitOutcome::Timeout;
    }
    const std::chrono::milliseconds slice = std::min(kSlice, remaining);
    (void)mutable_second.waitFor(slice);
    remaining -= slice;
  }
}
#endif
} // namespace sonitude::rt
