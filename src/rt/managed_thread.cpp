#include "rt/managed_thread.hpp"

#include <algorithm>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <limits.h>
#include <sched.h>
#endif

namespace sonitude::rt
{
StartGate::StartGate(const std::size_t expected_threads) : expected_(expected_threads) {}

bool StartGate::arriveAndWait()
{
  // Release: everything this thread published about itself before arriving must
  // be visible to the supervisor once it observes the arrival.
  arrived_.fetch_add(1, std::memory_order_release);
  arrival_event_.signal();

  for (;;)
  {
    const State state = static_cast<State>(state_.load(std::memory_order_acquire));
    if (state == State::Released)
    {
      return true;
    }
    if (state == State::Aborted)
    {
      return false;
    }
    // The release event is sticky, so a decision taken between the load above
    // and this wait still returns immediately. The bound is a liveness backstop
    // only; a supervisor that never decides is a startup bug, not a stall to be
    // tolerated silently.
    (void)release_event_.waitFor(std::chrono::milliseconds(1000));
  }
}

bool StartGate::waitForAll(const std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;)
  {
    if (arrived_.load(std::memory_order_acquire) >= expected_)
    {
      return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
    {
      return arrived_.load(std::memory_order_acquire) >= expected_;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    (void)arrival_event_.waitFor(remaining);
    // Auto-reset: clear the token so the next arrival is observed as a new edge.
    (void)arrival_event_.consume();
  }
}

void StartGate::release()
{
  state_.store(static_cast<std::uint8_t>(State::Released), std::memory_order_release);
  release_event_.signal();
}

void StartGate::abort()
{
  state_.store(static_cast<std::uint8_t>(State::Aborted), std::memory_order_release);
  release_event_.signal();
}

std::size_t StartGate::arrived() const
{
  return arrived_.load(std::memory_order_acquire);
}

ManagedThread::~ManagedThread()
{
  join();
}

void* ManagedThread::Trampoline(void* const self) noexcept
{
  static_cast<ManagedThread*>(self)->run();
  return nullptr;
}

void ManagedThread::run() noexcept
{
  SetCurrentThreadName(spec_.name);
  // Fault in the stack we expect to use while it is still free to be slow.
  PrefaultStack(spec_.prefault_bytes);
  // Report what the kernel actually gave this thread, never what was asked for.
  status_.observed = ObserveCurrentThreadScheduling();

  if (gate_ != nullptr && !gate_->arriveAndWait())
  {
    return;
  }
  if (body_)
  {
    body_();
  }
}

#if defined(__linux__)
int ManagedThread::createWith(const SchedClass sched_class, const std::int32_t priority) noexcept
{
  pthread_attr_t attr{};
  int rc = pthread_attr_init(&attr);
  if (rc != 0)
  {
    return rc;
  }

  const int policy = (sched_class == SchedClass::Realtime) ? SCHED_FIFO : SCHED_OTHER;
  sched_param param{};
  param.sched_priority = priority;

  // PTHREAD_EXPLICIT_SCHED is the point of this whole function: without it the
  // new thread would inherit the creator's policy and could execute at the
  // creator's realtime priority before anything demoted it.
  rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  if (rc == 0)
  {
    rc = pthread_attr_setschedpolicy(&attr, policy);
  }
  if (rc == 0)
  {
    rc = pthread_attr_setschedparam(&attr, &param);
  }
  if (rc == 0 && spec_.stack_bytes > 0)
  {
#if defined(PTHREAD_STACK_MIN)
    const std::size_t floor_bytes = static_cast<std::size_t>(PTHREAD_STACK_MIN);
#else
    const std::size_t floor_bytes = 128U * 1024U;
#endif
    rc = pthread_attr_setstacksize(&attr, std::max(spec_.stack_bytes, floor_bytes));
  }
  if (rc == 0)
  {
    rc = pthread_create(&handle_, &attr, &ManagedThread::Trampoline, this);
  }
  (void)pthread_attr_destroy(&attr);
  return rc;
}

bool ManagedThread::start(const ThreadSpec& spec, StartGate* const gate, Body body)
{
  spec_ = spec;
  gate_ = gate;
  body_ = std::move(body);
  status_ = ThreadStatus{};
  status_.name = (spec.name != nullptr) ? spec.name : "sonitude";

  // Reject an out-of-range realtime priority up front rather than silently
  // clamping it: the caller must see that its configuration is wrong.
  if (!IsPriorityValid(spec.sched_class, spec.priority))
  {
    status_.create_error = EINVAL;
    return false;
  }

  int rc = createWith(spec.sched_class, spec.priority);
  if (rc == 0)
  {
    started_ = true;
    return true;
  }

  const bool may_degrade = spec.sched_class == SchedClass::Realtime && !spec.require_realtime &&
                           (rc == EPERM || rc == EACCES);
  if (!may_degrade)
  {
    status_.create_error = rc;
    return false;
  }

  // Written before the second create so the worker cannot be observing the
  // status object while it is being modified.
  status_.degraded = true;
  status_.create_error = rc;
  const int fallback_rc = createWith(SchedClass::Normal, 0);
  if (fallback_rc != 0)
  {
    status_.degraded = false;
    status_.create_error = fallback_rc;
    return false;
  }
  started_ = true;
  return true;
}

void ManagedThread::join()
{
  if (started_)
  {
    (void)pthread_join(handle_, nullptr);
    started_ = false;
  }
}
#else
int ManagedThread::createWith(SchedClass, std::int32_t) noexcept
{
  return 0;
}

bool ManagedThread::start(const ThreadSpec& spec, StartGate* const gate, Body body)
{
  spec_ = spec;
  gate_ = gate;
  body_ = std::move(body);
  status_ = ThreadStatus{};
  status_.name = (spec.name != nullptr) ? spec.name : "sonitude";
  if (spec.require_realtime)
  {
    // No portable way to guarantee a realtime policy off Linux.
    return false;
  }
  status_.degraded = spec.sched_class == SchedClass::Realtime;
  fallback_ = std::thread([this] { run(); });
  started_ = true;
  return true;
}

void ManagedThread::join()
{
  if (started_ && fallback_.joinable())
  {
    fallback_.join();
  }
  started_ = false;
}
#endif
}  // namespace sonitude::rt
