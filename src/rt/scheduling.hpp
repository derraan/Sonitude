#pragma once

#include <cstddef>
#include <cstdint>

namespace sonitude::rt
{
enum class SchedClass : std::uint8_t
{
  // SCHED_OTHER: everything that may allocate, log, parse, or touch the network.
  Normal = 0,
  // SCHED_FIFO: bounded, allocation-free, deadline-bearing work only.
  Realtime = 1,
};

const char* SchedClassName(SchedClass sched_class) noexcept;

// What a thread actually got, as observed by that thread about itself.
// Never inferred from what was requested.
struct SchedObservation
{
  bool valid = false;
  int policy = -1;           // raw SCHED_* value
  std::int32_t priority = 0; // static priority (0 for SCHED_OTHER)
  bool realtime = false;     // policy is SCHED_FIFO or SCHED_RR
  std::uint64_t tid = 0;     // gettid(), for correlating with the kernel
  int error = 0;             // errno when valid == false
};

// Reads the calling thread's live scheduling parameters from the kernel.
SchedObservation ObserveCurrentThreadScheduling() noexcept;

// Valid static-priority range for the class, or {0,0} when not applicable.
struct PriorityRange
{
  std::int32_t min = 0;
  std::int32_t max = 0;
};
PriorityRange PriorityRangeFor(SchedClass sched_class) noexcept;

// True when priority is inside the kernel's permitted range for the class.
bool IsPriorityValid(SchedClass sched_class, std::int32_t priority) noexcept;

// Names the calling thread (truncated to the platform limit) so that ps/htop
// and the telemetry log agree.
void SetCurrentThreadName(const char* name) noexcept;

// Locks the process's current and future pages into RAM. On failure, writes the
// errno into out_error so the caller can report why (typically ENOMEM from
// RLIMIT_MEMLOCK, or EPERM).
bool TryEnableMemoryLocking(int* out_error) noexcept;

// Touches `bytes` of stack below the current frame so the pages are resident and
// write-faulted before any deadline applies. Call from the thread itself, once,
// before its first period. Does nothing when bytes == 0.
void PrefaultStack(std::size_t bytes) noexcept;
} // namespace sonitude::rt
