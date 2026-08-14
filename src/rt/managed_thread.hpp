#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "rt/scheduling.hpp"
#include "rt/wake_event.hpp"

#if defined(__linux__)
#include <pthread.h>
#else
#include <thread>
#endif

namespace sonitude::rt
{
// Startup rendezvous shared by every runtime thread.
//
// Invariant enforced here: a thread never runs its workload until the
// supervisor has inspected the scheduling parameters the kernel actually gave
// it. Each worker publishes its own observed policy, arrives at the gate, and
// blocks. The supervisor waits for all arrivals, validates them, and then either
// releases the gate (run) or aborts it (exit without running).
//
// Built from atomics and wake events rather than a mutex and condition variable.
// Two reasons: no realtime thread should ever have to take a std::mutex, not even
// once at startup; and the happens-before edge for the published status becomes
// explicit rather than incidental to a lock.
//
// Happens-before for ThreadStatus: the worker writes status_ and then performs
// arrived_.fetch_add(release). The supervisor observes arrived_.load(acquire) >=
// expected before reading any status, so every worker's write to its status is
// visible to the supervisor.
//
// The release event is deliberately never consumed, so it stays readable and
// every waiter -- including one that arrives late -- is released immediately
// rather than waiting out a timeout.
class StartGate
{
public:
  explicit StartGate(std::size_t expected_threads);

  // Worker side. Returns true when the workload should run, false when startup
  // was aborted and the thread must exit immediately.
  bool arriveAndWait();

  // Supervisor side. Returns false on timeout (a thread failed to arrive).
  bool waitForAll(std::chrono::milliseconds timeout);

  void release();
  void abort();

  std::size_t arrived() const;

private:
  enum class State : std::uint8_t
  {
    Waiting = 0,
    Released = 1,
    Aborted = 2,
  };

  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "the arrival count is observed by realtime threads and must be lock-free");
  static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                "the gate state is observed by realtime threads and must be lock-free");

  std::size_t expected_ = 0;
  std::atomic<std::size_t> arrived_{0};
  std::atomic<std::uint8_t> state_{static_cast<std::uint8_t>(State::Waiting)};
  WakeEvent arrival_event_; // worker -> supervisor, auto-reset
  WakeEvent release_event_; // supervisor -> workers, sticky broadcast
};

struct ThreadSpec
{
  // Kernel thread name; keep to 15 characters so it is not truncated.
  const char* name = "sonitude";
  SchedClass sched_class = SchedClass::Normal;
  std::int32_t priority = 0;
  // When true, failing to obtain the requested realtime policy is a startup
  // error rather than a degraded start.
  bool require_realtime = false;
  // 0 selects the platform default.
  std::size_t stack_bytes = 0;
  // Stack bytes touched by the thread before it arrives at the gate.
  std::size_t prefault_bytes = 0;
};

struct ThreadStatus
{
  std::string name;
  bool started = false;
  // Requested realtime but running as SCHED_OTHER (permitted only when
  // require_realtime is false).
  bool degraded = false;
  int create_error = 0;
  SchedObservation observed{};
};

// A thread whose final scheduling policy is established by the kernel at
// creation time (PTHREAD_EXPLICIT_SCHED plus attribute policy/priority), not
// patched afterwards from the creating thread. There is therefore no window in
// which the body runs under an inherited policy.
class ManagedThread
{
public:
  using Body = std::function<void()>;

  ManagedThread() = default;
  ~ManagedThread();
  ManagedThread(const ManagedThread&) = delete;
  ManagedThread& operator=(const ManagedThread&) = delete;

  // Creates the thread with explicit scheduling attributes. Returns false when
  // the thread could not be created, or when a required realtime policy was
  // refused; in both cases no thread is left running. The body does not start
  // until gate->release().
  bool start(const ThreadSpec& spec, StartGate* gate, Body body);

  void join();
  bool joinable() const noexcept
  {
    return started_;
  }

  // Valid once the thread has arrived at the gate.
  const ThreadStatus& status() const noexcept
  {
    return status_;
  }

private:
  static void* Trampoline(void* self) noexcept;
  void run() noexcept;
  int createWith(SchedClass sched_class, std::int32_t priority) noexcept;

  ThreadSpec spec_{};
  ThreadStatus status_{};
  StartGate* gate_ = nullptr;
  Body body_;
  bool started_ = false;
#if defined(__linux__)
  pthread_t handle_{};
#else
  std::thread fallback_;
#endif
};
} // namespace sonitude::rt
