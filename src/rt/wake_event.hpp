#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace sonitude::rt
{
// A single-slot wakeup token used to replace fixed-interval polling.
class WakeEvent
{
 public:
  WakeEvent();
  ~WakeEvent();
  WakeEvent(const WakeEvent&) = delete;
  WakeEvent& operator=(const WakeEvent&) = delete;

  void signal() noexcept;
  bool consume() noexcept;
  bool waitFor(std::chrono::milliseconds timeout) noexcept;
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

WaitOutcome WaitAnyOf(const WakeEvent& first,
                      const WakeEvent& second,
                      std::chrono::milliseconds timeout) noexcept;
}  // namespace sonitude::rt
