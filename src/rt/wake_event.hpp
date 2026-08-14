#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace sonitude::rt
{
// A single-slot wakeup token used to replace fixed-interval polling.
//
// On Linux this is an eventfd, which gives two properties this project needs:
//   * signal() is async-signal-safe (a single write(2) of 8 bytes to a
//     non-blocking descriptor), so a POSIX signal handler may call it;
//   * fd() can be polled together with ALSA's own poll descriptors, so a
//     realtime thread can block on "audio ready OR shutdown" in one syscall
//     instead of waking on a timer to re-check a flag.
//
// signal() never blocks and never allocates, so a realtime producer may call
// it. The non-Linux implementation is a mutex/condition-variable fallback that
// exists only to keep host tooling and non-Linux builds compiling; it is not
// used on any realtime path and fd() reports -1 there.
class WakeEvent
{
 public:
  WakeEvent();
  ~WakeEvent();
  WakeEvent(const WakeEvent&) = delete;
  WakeEvent& operator=(const WakeEvent&) = delete;

  // Wake one pending waiter (or arm the event for the next waiter).
  // Non-blocking, allocation-free, and safe from a signal handler on Linux.
  void signal() noexcept;

  // Clear any pending wakeup. Returns true when a wakeup was pending.
  bool consume() noexcept;

  // Block until signalled or until the timeout expires. Returns true if the
  // event was signalled. Does not consume the wakeup.
  bool waitFor(std::chrono::milliseconds timeout) noexcept;

  // Pollable descriptor, or -1 when this build has no descriptor backing.
  int fd() const noexcept { return fd_; }

 private:
  int fd_ = -1;
#if !defined(__linux__)
  std::mutex mutex_;
  std::condition_variable cv_;
  bool signalled_ = false;
#endif
};

enum class WaitOutcome : std::uint8_t
{
  Timeout = 0,
  First = 1,
  Second = 2,
  Error = 3,
};

// Blocks until either event is signalled or the timeout expires, in a single
// syscall. `second` wins when both are ready, which is what callers want: the
// second argument is the shutdown event, and shutdown takes precedence over
// more work. Neither event is consumed.
WaitOutcome WaitAnyOf(const WakeEvent& first,
                      const WakeEvent& second,
                      std::chrono::milliseconds timeout) noexcept;
}  // namespace sonitude::rt
