#include "rt/rt_thread.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

namespace sonitude::rt
{
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
  (void)mlockall(MCL_CURRENT | MCL_FUTURE);
  sched_param sch_params{};
  sch_params.sched_priority = priority;
  return pthread_setschedparam(thread.native_handle(), SCHED_FIFO, &sch_params) == 0;
#else
  (void)thread;
  (void)priority;
  return false;
#endif
}
}  // namespace sonitude::rt
