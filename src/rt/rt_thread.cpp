#include "rt/rt_thread.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

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
  return ConfigureThreadPolicy(thread.native_handle(), SCHED_FIFO, priority);
#else
  (void)thread;
  (void)priority;
  return false;
#endif
}

bool TryConfigureRtScheduling(std::jthread& thread, const std::int32_t priority)
{
#if defined(__linux__)
  return ConfigureThreadPolicy(thread.native_handle(), SCHED_FIFO, priority);
#else
  (void)thread;
  (void)priority;
  return false;
#endif
}

bool TryConfigureOtherScheduling(std::thread& thread)
{
#if defined(__linux__)
  return ConfigureThreadPolicy(thread.native_handle(), SCHED_OTHER, 0);
#else
  (void)thread;
  return false;
#endif
}

bool TryConfigureOtherScheduling(std::jthread& thread)
{
#if defined(__linux__)
  return ConfigureThreadPolicy(thread.native_handle(), SCHED_OTHER, 0);
#else
  (void)thread;
  return false;
#endif
}

bool TryConfigureCurrentThreadRtScheduling(const std::int32_t priority)
{
#if defined(__linux__)
  return ConfigureThreadPolicy(pthread_self(), SCHED_FIFO, priority);
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
}  // namespace sonitude::rt
