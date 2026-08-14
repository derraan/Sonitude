#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "rt/lifecycle.hpp"
#include "rt/managed_thread.hpp"
#include "rt/scheduling.hpp"
#include "rt/wake_event.hpp"

namespace
{
#if defined(__linux__)
volatile std::sig_atomic_t g_previous_handler_calls = 0;

void PreviousSignalHandler(int)
{
  g_previous_handler_calls = 1;
}
#endif

void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestWakeEventSignalAndConsume()
{
  sonitude::rt::WakeEvent event;
  Require(!event.consume(), "a fresh event must have no pending wakeup");
  event.signal();
  Require(event.waitFor(std::chrono::milliseconds(0)), "a signalled event must report ready");
  Require(event.consume(), "consume must report the pending wakeup");
  Require(!event.consume(), "consume must clear the wakeup");
  Require(!event.waitFor(std::chrono::milliseconds(0)),
          "a consumed event must not report ready");
}

void TestWakeEventReleasesBlockedWaiter()
{
  sonitude::rt::WakeEvent event;
  std::latch waiter_ready(1);
  std::atomic<bool> woke{false};

  std::thread waiter([&] {
    waiter_ready.count_down();
    // Generous bound: the assertion is that signal() releases it, and the test
    // fails if it only returns because the bound expired.
    woke.store(event.waitFor(std::chrono::milliseconds(5000)), std::memory_order_release);
  });

  waiter_ready.wait();
  event.signal();
  waiter.join();
  Require(woke.load(std::memory_order_acquire), "signal must release a blocked waiter");
}

void TestStopIsStickyAndFirstReasonWins()
{
  sonitude::rt::Lifecycle lifecycle;
  Require(!lifecycle.stopRequested(), "lifecycle must start running");
  Require(lifecycle.reason() == sonitude::rt::StopReason::None, "initial reason must be None");

  lifecycle.requestStop(sonitude::rt::StopReason::PlaybackFailure);
  Require(lifecycle.stopRequested(), "stop must be observable immediately");
  Require(lifecycle.reason() == sonitude::rt::StopReason::PlaybackFailure,
          "the first reason must be recorded");

  lifecycle.requestStop(sonitude::rt::StopReason::Signal);
  Require(lifecycle.reason() == sonitude::rt::StopReason::PlaybackFailure,
          "a later reason must not overwrite the first");
  Require(lifecycle.stopRequested(), "stop must stay requested");

  // Sticky: a thread that checks late still sees it, without waiting out the period.
  const auto before = std::chrono::steady_clock::now();
  Require(lifecycle.waitForStopOr(std::chrono::milliseconds(5000)),
          "waitForStopOr must return true once stop is requested");
  const auto elapsed = std::chrono::steady_clock::now() - before;
  Require(elapsed < std::chrono::milliseconds(2000),
          "waitForStopOr must return promptly after stop, not wait out its period");
}

void TestStopReleasesConcurrentWaiters()
{
  sonitude::rt::Lifecycle lifecycle;
  constexpr int kWaiters = 4;
  std::latch ready(kWaiters);
  std::atomic<int> observed{0};

  std::vector<std::thread> waiters;
  waiters.reserve(kWaiters);
  for (int i = 0; i < kWaiters; ++i)
  {
    waiters.emplace_back([&] {
      ready.count_down();
      if (lifecycle.waitForStopOr(std::chrono::milliseconds(5000)))
      {
        observed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  ready.wait();
  lifecycle.requestStop(sonitude::rt::StopReason::Signal);
  for (std::thread& waiter : waiters)
  {
    waiter.join();
  }
  Require(observed.load(std::memory_order_relaxed) == kWaiters,
          "every waiter must observe a single stop request");
}

#if defined(__linux__)
void TestSignalInstallationDeliveryTeardownAndRestoration()
{
  struct sigaction original_int
  {
  };
  struct sigaction original_term
  {
  };
  Require(::sigaction(SIGINT, nullptr, &original_int) == 0,
          "the existing SIGINT handler must be readable");
  Require(::sigaction(SIGTERM, nullptr, &original_term) == 0,
          "the existing SIGTERM handler must be readable");

  struct sigaction previous
  {
  };
  previous.sa_handler = PreviousSignalHandler;
  (void)::sigemptyset(&previous.sa_mask);
  previous.sa_flags = 0;
  Require(::sigaction(SIGINT, &previous, nullptr) == 0,
          "the test SIGINT handler must install");
  Require(::sigaction(SIGTERM, &previous, nullptr) == 0,
          "the test SIGTERM handler must install");

  {
    sonitude::rt::Lifecycle lifecycle;
    {
      sonitude::rt::SignalHandlerInstallation installation(lifecycle);
      Require(installation.installed(), "SIGINT/SIGTERM installation must succeed");
      Require(::raise(SIGINT) == 0, "SIGINT delivery must succeed");
      Require(lifecycle.stopRequested(), "SIGINT must request lifecycle stop");
      Require(lifecycle.reason() == sonitude::rt::StopReason::Signal,
              "SIGINT must preserve the signal stop reason");
      lifecycle.requestStop(sonitude::rt::StopReason::PlaybackFailure);
      Require(lifecycle.reason() == sonitude::rt::StopReason::Signal,
              "a later stop must not replace the signal reason");
    }

    struct sigaction restored_int
    {
    };
    struct sigaction restored_term
    {
    };
    Require(::sigaction(SIGINT, nullptr, &restored_int) == 0,
            "the restored SIGINT handler must be readable");
    Require(::sigaction(SIGTERM, nullptr, &restored_term) == 0,
            "the restored SIGTERM handler must be readable");
    Require(restored_int.sa_handler == PreviousSignalHandler,
            "teardown must restore the previous SIGINT handler");
    Require(restored_term.sa_handler == PreviousSignalHandler,
            "teardown must restore the previous SIGTERM handler");

    g_previous_handler_calls = 0;
    Require(::raise(SIGTERM) == 0, "restored SIGTERM delivery must succeed");
    Require(g_previous_handler_calls == 1,
            "a signal after teardown must reach the previous handler");
  }

  (void)::sigaction(SIGTERM, &original_term, nullptr);
  (void)::sigaction(SIGINT, &original_int, nullptr);
}
#endif

// The gate is the mechanism that stops a thread from running before its policy
// is settled, so it is tested directly rather than only through main().
void TestStartGateHoldsWorkUntilRelease()
{
  sonitude::rt::StartGate gate(2);
  std::atomic<int> ran{0};
  sonitude::rt::ManagedThread first;
  sonitude::rt::ManagedThread second;

  const sonitude::rt::ThreadSpec spec{.name = "gate-test"};
  Require(first.start(spec, &gate, [&] { ran.fetch_add(1, std::memory_order_relaxed); }),
          "gate test thread one must start");
  Require(second.start(spec, &gate, [&] { ran.fetch_add(1, std::memory_order_relaxed); }),
          "gate test thread two must start");

  Require(gate.waitForAll(std::chrono::milliseconds(5000)),
          "both threads must reach the gate");
  // Both threads are parked at the gate and have published their status. Neither
  // body may have run yet.
  Require(ran.load(std::memory_order_relaxed) == 0,
          "no workload may run before the gate is released");
  Require(first.status().observed.valid, "a gated thread must report its own policy");
  Require(first.status().observed.tid != 0, "a gated thread must report its kernel id");

  gate.release();
  first.join();
  second.join();
  Require(ran.load(std::memory_order_relaxed) == 2, "both bodies must run after release");
}

void TestStartGateAbortSkipsWork()
{
  sonitude::rt::StartGate gate(1);
  std::atomic<bool> ran{false};
  sonitude::rt::ManagedThread thread;
  const sonitude::rt::ThreadSpec spec{.name = "gate-abort"};
  Require(thread.start(spec, &gate, [&] { ran.store(true, std::memory_order_relaxed); }),
          "abort test thread must start");
  Require(gate.waitForAll(std::chrono::milliseconds(5000)), "thread must reach the gate");
  gate.abort();
  thread.join();
  Require(!ran.load(std::memory_order_relaxed),
          "an aborted startup must not run the workload");
}

void TestSlowThreadIsNeverRealtime()
{
  // The regression under test: a thread created by a realtime creator used to
  // inherit the creator's policy. ManagedThread requests SCHED_OTHER explicitly,
  // so a Normal thread must come up non-realtime regardless of who created it.
  sonitude::rt::StartGate gate(1);
  sonitude::rt::ManagedThread thread;
  const sonitude::rt::ThreadSpec spec{.name = "slow-check"};
  Require(thread.start(spec, &gate, [] {}), "slow thread must start");
  Require(gate.waitForAll(std::chrono::milliseconds(5000)), "slow thread must reach the gate");

  const sonitude::rt::ThreadStatus& status = thread.status();
  Require(status.observed.valid, "slow thread must report its policy");
  Require(!status.observed.realtime,
          "a thread requested as SCHED_OTHER must not be running a realtime policy");
  Require(status.observed.priority == 0, "a SCHED_OTHER thread must have static priority 0");
  gate.release();
  thread.join();
}

// Realtime scheduling needs privileges the test runner usually lacks. The
// contract under test is therefore the fallback contract, which must hold either
// way: with permission the thread is realtime at the requested priority, and
// without permission it is explicitly marked degraded rather than silently
// pretending to be realtime.
void TestRealtimeRequestFallbackIsExplicit()
{
  sonitude::rt::StartGate gate(1);
  sonitude::rt::ManagedThread thread;
  const sonitude::rt::PriorityRange range =
      sonitude::rt::PriorityRangeFor(sonitude::rt::SchedClass::Realtime);
  if (range.max <= 0)
  {
    return;  // no realtime policy on this platform
  }

  const sonitude::rt::ThreadSpec spec{.name = "rt-check",
                                      .sched_class = sonitude::rt::SchedClass::Realtime,
                                      .priority = range.min,
                                      .require_realtime = false,
                                      .stack_bytes = 256U * 1024U,
                                      .prefault_bytes = 32U * 1024U};
  Require(thread.start(spec, &gate, [] {}), "a non-mandatory realtime request must still start");
  Require(gate.waitForAll(std::chrono::milliseconds(5000)), "realtime thread must reach the gate");

  const sonitude::rt::ThreadStatus& status = thread.status();
  Require(status.observed.valid, "realtime thread must report its policy");
  if (status.degraded)
  {
    Require(!status.observed.realtime,
            "a degraded thread must actually be running SCHED_OTHER");
    Require(status.create_error != 0, "a degraded start must record why realtime was refused");
  }
  else
  {
    Require(status.observed.realtime, "a non-degraded realtime thread must run SCHED_FIFO");
    Require(status.observed.priority == range.min,
            "a non-degraded realtime thread must run at the requested priority");
  }
  gate.release();
  thread.join();
}

void TestMandatoryRealtimeFailsRatherThanDegrades()
{
  const sonitude::rt::PriorityRange range =
      sonitude::rt::PriorityRangeFor(sonitude::rt::SchedClass::Realtime);
  if (range.max <= 0)
  {
    return;
  }
  sonitude::rt::StartGate gate(1);
  sonitude::rt::ManagedThread thread;
  const sonitude::rt::ThreadSpec spec{.name = "rt-required",
                                      .sched_class = sonitude::rt::SchedClass::Realtime,
                                      .priority = range.min,
                                      .require_realtime = true};
  const bool started = thread.start(spec, &gate, [] {});
  if (!started)
  {
    // Unprivileged: refused outright, with a reason, and no thread left behind.
    Require(thread.status().create_error != 0, "a refused mandatory start must record errno");
    Require(!thread.joinable(), "a refused start must not leave a thread running");
    return;
  }
  Require(gate.waitForAll(std::chrono::milliseconds(5000)), "thread must reach the gate");
  Require(!thread.status().degraded, "a mandatory realtime thread must never be degraded");
  Require(thread.status().observed.realtime,
          "a mandatory realtime thread that started must be realtime");
  gate.release();
  thread.join();
}

void TestInvalidRealtimePriorityIsRejected()
{
  sonitude::rt::StartGate gate(1);
  sonitude::rt::ManagedThread thread;
  const sonitude::rt::ThreadSpec spec{.name = "rt-bad-prio",
                                      .sched_class = sonitude::rt::SchedClass::Realtime,
                                      .priority = 100000};
  Require(!thread.start(spec, &gate, [] {}),
          "an out-of-range realtime priority must be rejected, not clamped");
  Require(!thread.joinable(), "a rejected start must not create a thread");
}

void TestPriorityValidation()
{
  const sonitude::rt::PriorityRange rt =
      sonitude::rt::PriorityRangeFor(sonitude::rt::SchedClass::Realtime);
  if (rt.max > 0)
  {
    Require(sonitude::rt::IsPriorityValid(sonitude::rt::SchedClass::Realtime, rt.min),
            "the minimum realtime priority must validate");
    Require(sonitude::rt::IsPriorityValid(sonitude::rt::SchedClass::Realtime, rt.max),
            "the maximum realtime priority must validate");
    Require(!sonitude::rt::IsPriorityValid(sonitude::rt::SchedClass::Realtime, rt.max + 1),
            "an above-range realtime priority must not validate");
  }
  Require(sonitude::rt::IsPriorityValid(sonitude::rt::SchedClass::Normal, 0),
          "SCHED_OTHER must accept priority 0");
  Require(!sonitude::rt::IsPriorityValid(sonitude::rt::SchedClass::Normal, 1),
          "SCHED_OTHER must reject a non-zero static priority");
}

void TestPrefaultStackIsHarmless()
{
  // Only checks that the helper runs and returns; its effect is a residency
  // property that cannot be observed portably from user space.
  sonitude::rt::PrefaultStack(0);
  sonitude::rt::PrefaultStack(64U * 1024U);
}
}  // namespace

void RunLifecycleTests()
{
  TestWakeEventSignalAndConsume();
  TestWakeEventReleasesBlockedWaiter();
  TestStopIsStickyAndFirstReasonWins();
  TestStopReleasesConcurrentWaiters();
#if defined(__linux__)
  TestSignalInstallationDeliveryTeardownAndRestoration();
#endif
  TestStartGateHoldsWorkUntilRelease();
  TestStartGateAbortSkipsWork();
  TestSlowThreadIsNeverRealtime();
  TestRealtimeRequestFallbackIsExplicit();
  TestMandatoryRealtimeFailsRatherThanDegrades();
  TestInvalidRealtimePriorityIsRejected();
  TestPriorityValidation();
  TestPrefaultStackIsHarmless();
}
