#include "rt/rt_thread.hpp"

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <iostream>

namespace sonitude::rt
{
namespace
{
#if defined(__linux__)
bool ConfigureThreadPolicy(const pthread_t handle, const int policy, const std::int32_t priority)
{
  sched_param sch_params{};
  sch_params.sched_priority = priority;
  return pthread_setschedparam(handle, policy, &sch_params) == 0;
}

const char* PolicyName(const int policy)
{
  switch (policy)
  {
    case SCHED_FIFO:
      return "SCHED_FIFO";
    case SCHED_RR:
      return "SCHED_RR";
    case SCHED_OTHER:
      return "SCHED_OTHER";
    default:
      return "SCHED_UNKNOWN";
  }
}

void PrintSchedulingForThread(const pthread_t handle, const char* label)
{
  int policy = 0;
  sched_param sch_params{};
  if (pthread_getschedparam(handle, &policy, &sch_params) == 0)
  {
    std::cout << "rt: " << label << " policy=" << PolicyName(policy)
              << " priority=" << sch_params.sched_priority << '\n';
  }
}

void PrintSchedulingForCurrentThread(const char* label)
{
  PrintSchedulingForThread(pthread_self(), label);
}

// One page-sized frame at a time, writing through volatile so the compiler
// cannot elide the touch, and noinline so the frames are distinct.
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void TouchStackFrame(const std::size_t remaining) noexcept
{
  constexpr std::size_t kFrameBytes = 4096;
  constexpr std::size_t kStrideBytes = 512;
  volatile unsigned char frame[kFrameBytes];
  unsigned char sink = 0;
  for (std::size_t offset = 0; offset < kFrameBytes; offset += kStrideBytes)
  {
    frame[offset] = 0;
    sink = static_cast<unsigned char>(sink | frame[offset]);
  }
  (void)sink;
  if (remaining > kFrameBytes)
  {
    TouchStackFrame(remaining - kFrameBytes);
  }
}
#endif
}  // namespace

RtThread::~RtThread()
{
  join();
}

void RtThread::start(const std::function<void()>& fn, const std::int32_t priority)
{
  worker_ = std::thread(fn);
  (void)TryConfigureRtScheduling(worker_, priority);
}

void RtThread::join()
{
  if (worker_.joinable())
  {
    worker_.join();
  }
}

bool TryConfigureRtScheduling(std::thread& thread, const std::int32_t priority)
{
#if defined(__linux__)
  const bool ok = ConfigureThreadPolicy(thread.native_handle(), SCHED_FIFO, priority);
  if (ok)
  {
    PrintSchedulingForThread(thread.native_handle(), "thread");
  }
  return ok;
#else
  (void)thread;
  (void)priority;
  return false;
#endif
}

bool TryConfigureRtScheduling(std::jthread& thread, const std::int32_t priority)
{
#if defined(__linux__)
  const bool ok = ConfigureThreadPolicy(thread.native_handle(), SCHED_FIFO, priority);
  if (ok)
  {
    PrintSchedulingForThread(thread.native_handle(), "jthread");
  }
  return ok;
#else
  (void)thread;
  (void)priority;
  return false;
#endif
}

bool TryConfigureOtherScheduling(std::thread& thread)
{
#if defined(__linux__)
  const bool ok = ConfigureThreadPolicy(thread.native_handle(), SCHED_OTHER, 0);
  if (ok)
  {
    PrintSchedulingForThread(thread.native_handle(), "thread");
  }
  return ok;
#else
  (void)thread;
  return false;
#endif
}

bool TryConfigureOtherScheduling(std::jthread& thread)
{
#if defined(__linux__)
  const bool ok = ConfigureThreadPolicy(thread.native_handle(), SCHED_OTHER, 0);
  if (ok)
  {
    PrintSchedulingForThread(thread.native_handle(), "jthread");
  }
  return ok;
#else
  (void)thread;
  return false;
#endif
}

bool TryConfigureCurrentThreadRtScheduling(const std::int32_t priority)
{
#if defined(__linux__)
  const bool ok = ConfigureThreadPolicy(pthread_self(), SCHED_FIFO, priority);
  if (ok)
  {
    PrintSchedulingForCurrentThread("current-thread");
  }
  return ok;
#else
  (void)priority;
  return false;
#endif
}

bool TryEnableMemoryLocking()
{
#if defined(__linux__)
  return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
#else
  return false;
#endif
}

void PrefaultStack(const std::size_t bytes) noexcept
{
#if defined(__linux__)
  if (bytes == 0)
  {
    return;
  }
  TouchStackFrame(bytes);
#else
  (void)bytes;
#endif
}
}  // namespace sonitude::rt
