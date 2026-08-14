#include "rt/scheduling.hpp"

#if defined(__linux__)
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace sonitude::rt
{
namespace
{
#if defined(__linux__)
int PolicyFor(const SchedClass sched_class) noexcept
{
  return sched_class == SchedClass::Realtime ? SCHED_FIFO : SCHED_OTHER;
}
#endif

// One page-sized frame at a time, writing through volatile so the compiler
// cannot elide the touch, and noinline so the frames are really distinct.
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void
TouchStackFrame(const std::size_t remaining) noexcept
{
  constexpr std::size_t kFrameBytes = 4096;
  constexpr std::size_t kStrideBytes = 512;  // <= any supported page size
  volatile unsigned char frame[kFrameBytes];
  unsigned char sink = 0;
  for (std::size_t offset = 0; offset < kFrameBytes; offset += kStrideBytes)
  {
    frame[offset] = 0;
    // Read back so the write cannot be optimised away as dead.
    sink = static_cast<unsigned char>(sink | frame[offset]);
  }
  (void)sink;
  if (remaining > kFrameBytes)
  {
    TouchStackFrame(remaining - kFrameBytes);
  }
}
}  // namespace

const char* SchedClassName(const SchedClass sched_class) noexcept
{
  return sched_class == SchedClass::Realtime ? "SCHED_FIFO" : "SCHED_OTHER";
}

SchedObservation ObserveCurrentThreadScheduling() noexcept
{
  SchedObservation out{};
#if defined(__linux__)
  int policy = 0;
  sched_param param{};
  const int rc = pthread_getschedparam(pthread_self(), &policy, &param);
  if (rc != 0)
  {
    out.error = rc;
    return out;
  }
  out.valid = true;
  out.policy = policy;
  out.priority = param.sched_priority;
  out.realtime = (policy == SCHED_FIFO) || (policy == SCHED_RR);
  out.tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
  return out;
}

PriorityRange PriorityRangeFor(const SchedClass sched_class) noexcept
{
#if defined(__linux__)
  const int policy = PolicyFor(sched_class);
  const int min = sched_get_priority_min(policy);
  const int max = sched_get_priority_max(policy);
  if (min < 0 || max < 0)
  {
    return {};
  }
  return {min, max};
#else
  (void)sched_class;
  return {};
#endif
}

bool IsPriorityValid(const SchedClass sched_class, const std::int32_t priority) noexcept
{
  if (sched_class == SchedClass::Normal)
  {
    return priority == 0;
  }
  const PriorityRange range = PriorityRangeFor(sched_class);
  return range.max > 0 && priority >= range.min && priority <= range.max;
}

void SetCurrentThreadName(const char* name) noexcept
{
#if defined(__linux__)
  if (name != nullptr)
  {
    // Silently truncated by the kernel at 16 bytes including the terminator.
    (void)pthread_setname_np(pthread_self(), name);
  }
#else
  (void)name;
#endif
}

bool TryEnableMemoryLocking(int* const out_error) noexcept
{
#if defined(__linux__)
  if (::mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
  {
    return true;
  }
  if (out_error != nullptr)
  {
    *out_error = errno;
  }
  return false;
#else
  if (out_error != nullptr)
  {
    *out_error = 0;
  }
  return false;
#endif
}

void PrefaultStack(const std::size_t bytes) noexcept
{
  if (bytes == 0)
  {
    return;
  }
  TouchStackFrame(bytes);
}
}  // namespace sonitude::rt
