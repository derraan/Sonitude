#pragma once

#include <cstdint>
#include <functional>
#include <thread>

namespace sonitude::rt
{
class RtThread
{
 public:
  RtThread() = default;
  ~RtThread();
  RtThread(const RtThread&) = delete;
  RtThread& operator=(const RtThread&) = delete;

  void start(const std::function<void()>& fn, std::int32_t priority);
  void join();

 private:
  std::thread worker_;
};

bool TryConfigureRtScheduling(std::thread& thread, std::int32_t priority);
bool TryConfigureRtScheduling(std::jthread& thread, std::int32_t priority);
bool TryConfigureOtherScheduling(std::thread& thread);
bool TryConfigureOtherScheduling(std::jthread& thread);
bool TryConfigureCurrentThreadRtScheduling(std::int32_t priority);
bool TryEnableMemoryLocking();
}  // namespace sonitude::rt
