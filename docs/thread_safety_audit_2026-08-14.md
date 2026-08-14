# Thread-safety and thread-management audit — Sonitude realtime runtime

Date: 2026-08-14
Reviewed code state: `audit/pr1-runtime-hardening` (PR #23), with the control-path
types from PR #24 taken into account where they cross the realtime boundary.
Remediation branch: `audit/thread-safety-2026-08-14`, based on commit `07acc8f`.
Scope: thread inventory, scheduling, cross-thread ownership, memory-model
correctness, shutdown, and the observability needed to qualify the above on
hardware. Signal-processing algorithms were not modified.

**Branch-point note, and it is load-bearing.** While this audit was in progress,
commit `2e59349` ("Fix playback ownership, occupancy, and RT snapshot contracts")
was pushed to PR #23. The remediation branch is based on its parent, `07acc8f`,
so that commit is not an ancestor of this work. It was reviewed separately, and it
does **not** close the findings its message implies — see the box at the top of
section B. Section B's defect list is stated against `07acc8f`; where `2e59349`
changes the picture, that is called out explicitly.

Verification platform: x86-64 Linux (WSL2, Ubuntu 22.04, GCC 11.4), non-PREEMPT_RT,
without the ALSA devices. Everything in section F was verified there. Nothing in
this report was verified on the Raspberry Pi target; see section G.

---

## A. Thread inventory

### A.1 As audited (commit `07acc8f`, before remediation)

| # | Thread | Created at | Creator | Policy as created | Final policy | Stop mechanism |
|---|--------|-----------|---------|-------------------|--------------|----------------|
| 1 | main / capture+DSP | process entry | kernel | SCHED_OTHER, self-promoted to FIFO 80 | FIFO 80 | `g_running` |
| 2 | control | `main.cpp`, after main self-promoted | main (FIFO 80) | **inherited FIFO 80** | demoted to SCHED_OTHER from inside the thread | `stop_token` + `g_running` |
| 3 | telemetry | `main.cpp` | main (FIFO 80) | **inherited FIFO 80** | demoted to SCHED_OTHER | `stop_token` |
| 4 | playback | `main.cpp` | main (FIFO 80) | **inherited FIFO 80** | re-set to FIFO 78 | `g_running` |
| 5 | capture unblock helper | `main.cpp` | main (FIFO 80) | **inherited FIFO 80** | **never demoted** | `g_running`, 1 ms poll |

Threads 2–5 each ran an unbounded amount of their own startup work at the
capture thread's realtime priority. Thread 5 ran its entire life there, and its
entire life was a `sleep_for(1 ms)` loop.

### A.2 As implemented (after remediation)

All four workers are created by the supervisor while the supervisor is still
SCHED_OTHER, with the final policy applied by the kernel at creation time, and
none of them runs a single line of its workload until the supervisor has
inspected what the kernel actually granted.

**Supervisor (`main`)** — role: initialisation, scheduling validation, teardown.
Policy SCHED_OTHER throughout; it is never promoted, so there is no policy for a
child to inherit. Blocking points: `Lifecycle::waitForStopOr` (200 ms slices) and
`pthread_join`. Owns: both `AlsaPcmDevice` objects, every DSP object, the
`BlockChannel`, the `SteeringChannel`, the `Lifecycle`, the `StartGate`, and all
four `ManagedThread`s. Allocates freely — all of it before the gate opens.
Endpoints: `Lifecycle` (writer of the stop decision's consequences, reader of the
reason), `StartGate` (supervisor side), `ThreadStatus` of each thread.

**capture/DSP** — role: ALSA capture, calibration, beamforming, suppression,
limiting, block publication. Policy SCHED_FIFO, priority `realtime.capture_priority`
(default 80). Blocking points: exactly one, `poll()` over ALSA's capture
descriptors plus the lifecycle wake descriptor, bounded by `wait_timeout_ms`
(8 nominal periods). No mutex, no condition variable, no allocation, and no
logging. The remaining `throw` sites reachable from this loop are bounds checks
over the channel map and the destination span; the map and the negotiated
container are validated once in the `CaptureWorker` constructor and cannot change
afterwards, and `readBlock` checks the span size itself and counts a refusal, so
the throws are unreachable in steady state rather than merely improbable.
Owns exclusively: `mic_frames`, `calibrated_frames`,
`mono`, `distractor_reference`, `stereo`, the beamformer, the suppressors, the
limiter, the calibration applier, and any block slot it currently holds.
Endpoints: `SteeringChannel` (consumer), `BlockChannel` (producer), `playback_wake`
(signaller), `TelemetryCounters` (relaxed stores only), `Lifecycle` (reader, and
writer of `CaptureFailure` / `CaptureStreamEnded`). Nominal period 1.451 ms at the
default 64 frames / 44.1 kHz. A missed deadline is directly audible.

**playback** — role: ASRC and ALSA playback writes. Policy SCHED_FIFO, priority
`realtime.playback_priority` (default 78). Blocking points: `snd_pcm_writei` on a
blocking PCM handle, and `WaitAnyOf(playback_wake, lifecycle)` when no block is
ready. No mutex, no condition variable, no allocation, no logging. Owns
exclusively: the resampler scratch buffers, the `AsrcController`, and any block it
currently holds. Endpoints: `BlockChannel` (consumer), `playback_wake` (waiter),
`TelemetryCounters`, `Lifecycle` (reader, and writer of `PlaybackFailure`).
Nominal period matches capture. A missed deadline is directly audible.

**control** — role: DOA ingestion, conversation state machine, steering
publication. Policy SCHED_OTHER. Blocking points: the DOA provider's socket I/O
(including `getaddrinfo`/`connect` retries for a real ODAS endpoint) and
`Lifecycle::waitForStopOr` (10 ms). Allocates, parses strings, throws, and logs —
all permitted, because nothing realtime waits on it. Owns: the DOA provider, the
`ConversationStateMachine`, the `ZoneMap`. Endpoints: `SteeringChannel` (producer),
`TelemetryCounters`, `Lifecycle` (reader, and writer of `ControlFailure`).
Missing its period degrades steering freshness only; audio keeps running on the
last snapshot, and a stalled control loop cannot hold a beam because the thread
publishes a failsafe snapshot on the way out.

**telemetry** — role: the only thread that formats and prints runtime metrics.
Policy SCHED_OTHER. Blocking points: `Lifecycle::waitForStopOr`
(`telemetry.stats_period_ms`, default 1000 ms) and `std::cout`. Owns nothing.
Endpoints: read-only relaxed loads from `TelemetryCounters`, `BlockChannel`
accounting, and `SteeringChannel` counters. Missing its period loses log lines
and nothing else.

**capture unblock helper** — removed. See defect P1-1.

Library-created threads: none. ALSA `hw:` devices are used directly, with no
`plug`/`dmix` plugin thread, and the ODAS provider does its own socket I/O on the
control thread. `libsamplerate` is called synchronously.

---

## B. Confirmed defects

Every finding below was confirmed against the code, not inferred from the audit
brief. Where the brief's hypothesis was wrong, that is stated.

> ### Assessment of commit `2e59349`, the current tip of PR #23
>
> This commit is a genuine partial fix and must not be merged as a complete one.
>
> **P0-3 (block ownership): adequately addressed.** It replaces modulo slot reuse
> with a real free/filled queue pair and an explicit consumer release, which is
> the right shape.
>
> **P0-2 (the data race): NOT fixed, despite the commit message.** It removes
> `std::string zone_name` and adds
> `static_assert(std::is_trivially_copyable_v<SteeringSnapshot>)`, but leaves
> `src/rt/param_snapshot.hpp` — the seqlock — in the tree and in use as the
> control-to-audio handoff. Trivial copyability constrains the *message*; the race
> is in the *handoff*. The writer still assigns to a non-atomic object while the
> reader copies that same object, nothing orders the two accesses, and that is a
> data race and therefore undefined behaviour no matter how simple the type is.
> Removing the string does remove the worst *consequences* (a racing `std::string`
> copy can double-free or read a dangling pointer), which makes the remaining
> defect quieter and no less real.
>
> This is demonstrated, not asserted: `tests/race_repro/trivially_copyable_seqlock_race.cpp`
> is that exact combination — the trivially copyable snapshot published through the
> surviving seqlock, with the upstream `static_assert` present and passing — and
> ThreadSanitizer still reports a data race on the writer's `slots_[slot] = value`.
>
> **P0-4 (duplicated occupancy): weakened, not resolved.** `queued_frames_` remains
> a second publication of the queue's state, now as a single counter that both
> realtime threads read-modify-write. Underflow is no longer reachable (the
> producer's `fetch_add` is sequenced before the push it is acquired with), so the
> unsigned-wrap defect is genuinely gone. But the brief asked for a single source
> of truth, and this is still two, plus a contended atomic shared between the two
> realtime threads. It also relies on `assert` for its ownership and bounds
> invariants, and CMake's `RelWithDebInfo` defines `NDEBUG`, so every one of those
> checks — including the `writable()`/`readable()` slot bounds — compiles out of
> the build that would actually ship.
>
> **P0-1 (scheduler inheritance): untouched.** The commit does not change thread
> startup, so the inheritance window and the never-demoted FIFO 80 polling helper
> are both still present at the tip of PR #23.
>
> Recommendation: take `2e59349`'s intent, not its implementation. `BlockChannel`
> supersedes `PlaybackBlockPool` (owner state machine, contiguous storage,
> single-writer counters, checks that survive `NDEBUG`), and `SteeringChannel`
> supersedes the seqlock outright.

### P0 — correctness

**P0-1. Scheduler inheritance let non-realtime threads run at realtime priority.**
Confirmed exactly as hypothesised. `main` promoted itself to `SCHED_FIFO 80`
before creating any worker; `std::jthread` gives no way to set scheduling
attributes at creation, so every worker was created with the default
`PTHREAD_INHERIT_SCHED` and started life at FIFO 80. Each worker then demoted or
re-prioritised itself from inside its own body — after an unbounded amount of
already-running work. The capture unblock helper never demoted itself at all, so
a thread whose entire body was `sleep_for(1 ms)` held the highest realtime
priority in the process for the process's lifetime. On a single-core or
core-restricted Pi this is a plausible mechanism for capture starvation.

**P0-2. `SnapshotBuffer` was a data race, i.e. undefined behaviour.**
Confirmed. The writer performed `slots_[slot] = value` on an ordinary non-atomic
`SteeringSnapshot` while a reader performed `const T value = slots_[slot]` on the
same object. The sequence counter made the reader *discard* torn results; it did
not prevent the conflicting accesses from happening. Under [intro.races] two
conflicting non-atomic accesses that are not ordered by happens-before are a data
race, and a data race is UB — the program has no defined behaviour to salvage,
so "the reader retries afterwards" is not a defence. The kernel's seqlock is
sound only because it is written against a different (compiler-barrier plus
`READ_ONCE`) memory model that C++ does not give you.

This one is demonstrated rather than argued. `tests/race_repro/old_snapshot_race.cpp`
reconstructs the deleted header and drives it the way the runtime did;
ThreadSanitizer reports write/read races on the snapshot slot at
`SteeringSnapshot::operator=`.

Aggravating factor, also confirmed: the realtime-facing snapshot contained
`std::string zone_name`. The reader's copy therefore called `operator new` on the
audio thread every period, and the racing copy of a `std::string` is not merely
torn — it can double-free or read a dangling pointer.

**P0-3. Playback backing slots had no owner.**
Confirmed. `playback_block_pool.hpp` handed out slots by `index % slot_count` with
no consumer release step, so the producer would rewrite a slot while playback
still held a `PlaybackBlockRef` to it, or while that reference sat in the queue.
The corruption window scaled with playback lateness, which is precisely when it
would be hardest to diagnose.

**P0-4. `software_queued_frames` could disagree with the queue, and could underflow.**
Confirmed. Occupancy was published both by the block queue and by a separate
atomic counter incremented by the producer and decremented by the consumer, with
no ordering between them. A consumer that decremented before the producer's
increment became visible underflowed an unsigned counter to ~1.8e19, which then
fed the ASRC ratio estimator.

**P0-5 (new, found during remediation). `BlockChannel::abandon` violated its own
SPSC contract.** In the first cut of the replacement block channel, `abandon()` —
called on the producer thread — pushed the slot back into the free queue, which
playback also pushes to. Two producers on one SPSC ring corrupt the head index
and can duplicate or lose a slot, which would have reintroduced P0-3 by a
different route. Fixed before this report by keeping a relinquished slot in a
producer-private reserve; the producer now never touches the free queue's push
side. `TestAbandonDoesNotRaceConsumerRelease` fails deterministically if the
defective version is restored (verified by reintroducing it).

### P1 — realtime determinism

**P1-1. 1 ms polling loops on the same order as the audio deadline.**
Confirmed. At the default 64 frames / 44.1 kHz the period is 1.451 ms, so a 1 ms
poll is not "fast enough to be invisible", it is 69 % of the deadline. The
capture unblock helper polled at 1 ms *at FIFO 80* (P0-1), and playback's
prefill/starvation paths slept 1 ms rather than waiting on anything.

**P1-2. Capture could not be interrupted without a dedicated helper thread.**
Confirmed. `snd_pcm_readi` on a blocking handle was the only capture wait, so
shutdown needed a second thread whose only job was to notice `g_running` and drop
the stream.

**P1-3. Scheduling failures were silent.** `pthread_setschedparam` failures were
discarded, so a deployment without `CAP_SYS_NICE` ran the entire audio path at
SCHED_OTHER while printing nothing. There was no way to tell the two cases apart
from the logs.

**P1-4. Priorities 80/78 were asserted, not derived.** Confirmed, and still true —
see section H. No WCET measurement exists for this pipeline. The ordering
(capture above playback) is defensible as a starting point because capture has a
hard device deadline and feeds playback, but it is provisional and is now labelled
as such in both the header and the config.

**P1-5. `mlockall` failure was not reported, and RT stacks were not prefaulted.**
Confirmed. A failed `mlockall` left the process silently swappable, and each
worker's stack was faulted in lazily — the first deep call in the audio loop took
its page faults with a deadline already running.

**P1-6. `readBlock` could throw from the steady-state audio loop.** Confirmed: an
oversized destination threw `std::runtime_error`, which allocates during
unwinding, on the realtime thread. The bounds check is correct and was kept; only
the reporting mechanism changed — it now counts a refusal.

`ExtractActiveMicFrames`, called every period from the same loop, has three
further `throw` sites: a channel-map size check, a per-index range check against
the container channel count, and a destination-span size check. These are also
correct checks and were also kept. They are now provably unreachable from the
audio loop instead of being unreachable by argument: the first two are validated
once in the `CaptureWorker` constructor, against invariants that cannot change
after the device is open, and the third is subsumed by `readBlock`'s own check.

### P2 — maintainability

**P2-1. Shutdown had four overlapping authorities** (`g_running`, `std::stop_token`,
a condition variable, and ALSA drop), with no single place to read the shutdown
order from and no record of *why* the process was stopping.

**P2-2. `SpscRing` accepted non-trivially-copyable element types**, so nothing
prevented a future `std::string`-carrying message from being pushed across the
realtime boundary again.

**P2-3. `capacity()` was ambiguous** — it returned the slot count, one of which is
reserved, so callers sizing a pool from it were off by one.

**P2-4 (accepted, not fixed). `pendingFrames()` is not instantaneously exact.**
`commit()` bumps the committed-frame total before pushing the block, so a
consumer reading between those two stores counts one block it cannot yet pop. The
error is bounded by one period, is always in the direction of over-reporting
occupancy, cannot underflow, and feeds only the ASRC ratio estimate — never an
ownership decision. The alternative order (push, then count) would let the
consumer take a block before it was counted, which *can* underflow. The current
order is the safe one and the residual is documented in the header.

**P2-5 (accepted). `std::signal` rather than `sigaction`.** The handler itself is
async-signal-safe (a relaxed load of a lock-free atomic pointer, a
compare-exchange, and an 8-byte `write` to a non-blocking eventfd), so this is
correct; `sigaction` would merely make the flags explicit.

---

## C. Implemented changes

New primitives, all under `src/rt`:

- **`wake_event.hpp/.cpp`** — `WakeEvent`, an `eventfd`-backed wakeup token.
  `signal()` is a single non-blocking 8-byte `write`, which makes it both
  callable from a realtime thread and async-signal-safe. `fd()` is pollable, so a
  realtime thread can block on "audio ready OR shutdown" in one syscall.
  `WaitAnyOf` polls two events with shutdown given precedence. A
  mutex/condition-variable fallback exists for non-Linux host tooling and is
  documented as unusable on a realtime path.
- **`lifecycle.hpp/.cpp`** — `Lifecycle`, the single shutdown authority. Sticky
  stop flag, first-reason-wins `StopReason`, and a never-consumed `WakeEvent` so
  every waiter (including one that arrives after the decision) is released
  immediately. `InstallSignalHandlers` routes SIGINT/SIGTERM into it.
- **`scheduling.hpp/.cpp`** — policy observation (`ObserveCurrentThreadScheduling`
  reports what the kernel *gave* a thread, never what was asked for), priority
  range validation, thread naming, `mlockall`, and stack prefaulting.
- **`managed_thread.hpp/.cpp`** — `StartGate` and `ManagedThread`. This is the
  P0-1 fix: threads are created with `PTHREAD_EXPLICIT_SCHED` plus attribute
  policy and priority, so the kernel applies the final policy at creation and
  there is no inherited-policy window at all. `StartGate` is built from atomics
  and wake events rather than a mutex, so no realtime thread takes a lock even
  once at startup.
- **`block_channel.hpp`** — `BlockChannel`, the P0-3 and P0-4 fix. Explicit
  four-state ownership (`Free`/`Producer`/`Ready`/`Consumer`) over two SPSC rings,
  with occupancy *derived* from two single-writer monotone counters instead of
  published separately.

Control path:

- **`control/rt_steering_snapshot.hpp`** — `RtSteeringSnapshot`, trivially
  copyable with a `std::int16_t zone_id` in place of `std::string zone_name`,
  enforced by `static_assert`.
- **`control/steering_channel.hpp`** — `SteeringChannel`, a bounded SPSC queue
  with latest-wins draining. This is the P0-2 fix.
- `zones` gained numeric zone lookup so the control thread can resolve a name to
  an id on the slow side; `ControlLoop` publishes through the channel and has an
  explicit `publishFailsafe` for shutdown.
- **Deleted**: `rt/param_snapshot.hpp` (the seqlock), `rt/rt_thread.{hpp,cpp}`, and
  `tests/unit/snapshot_tests.cpp`. `audio/playback_block_pool.hpp` does not exist
  on this branch — it was introduced by `2e59349`, which is not an ancestor here;
  `rt/block_channel.hpp` supersedes it.

Audio path:

- `AlsaPcmDevice` exposes poll descriptors, `pollRevents`, `prepare`, and `start`.
- `CaptureWorker::prepareWaiting`/`waitForData` block on ALSA's descriptors plus
  the lifecycle wake descriptor. This removes both the 1 ms polling and the entire
  capture unblock helper thread (P1-1, P1-2).
- `CaptureWorker::readBlock` keeps its bounds check but counts a refusal instead
  of throwing, and the constructor now validates the channel map against the
  negotiated container so the extractor's own throws cannot be reached from the
  audio loop (P1-6).
- `SpscRing` gained `static_assert`s for trivial copyability and lock-free
  atomics, a corrected capacity contract (`capacityUsable()`), and an explicit
  happens-before comment (P2-2, P2-3).

Runtime:

- `main.cpp` restructured into four explicit phases — initialise (supervisor,
  SCHED_OTHER, all allocation), lifecycle and memory locking, start-and-validate,
  supervise-and-tear-down. The supervisor is never promoted to a realtime policy.
- Startup prints a per-thread scheduling table and then *validates* it: a slow
  thread found running realtime fails startup outright, and a realtime thread
  that fell back to SCHED_OTHER fails startup when `realtime.require_realtime` is
  set. Teardown joins in pipeline order and only then touches the devices.
- `realtime.require_realtime`, `rt_stack_kib`, `rt_prefault_kib`, and
  `startup_timeout_ms` added to config, loaded through an `OptionalScalar` helper
  so existing config files keep working.
- Telemetry extended with deadline/runtime statistics, high-water marks, pool
  exhaustion, starvation, control snapshot age, and publication drops.
- `scripts/run_sanitizer_tests.sh` builds and runs the suite under TSan and then
  ASan+UBSan.

---

## D. Memory-model justification

Every cross-thread handoff in the runtime, with the happens-before edge that
makes it defined.

**1. `SpscRing` (the substrate for the two below).** `head_` has exactly one
writer (the producer), `tail_` exactly one writer (the consumer), so neither
index can be lost. The producer refuses to advance `head_` into `tail_`, so the
element it writes is never the element the consumer reads: producer and consumer
never access the same `storage_` object, and the absence of a race on the payload
is structural, not probabilistic. Publication: the producer writes
`storage_[head]` then `head_.store(release)`; the consumer's `head_.load(acquire)`
synchronises with it, so the element write happens-before the element read.
Reclamation: the consumer reads `storage_[tail]` then `tail_.store(release)`; the
producer's `tail_.load(acquire)` synchronises with it, so the consumer's read
happens-before the producer's next write to that slot.

**2. Control → audio steering (`SteeringChannel`).** The payload is trivially
copyable and self-contained, so a slot copy is a byte copy with no ownership
transfer and no allocation. The edge is the ring's release/acquire pair from (1).
The audio thread drains to the newest message at a period boundary; superseded
messages are discardable because each snapshot is a complete description of the
desired state rather than a delta. When the queue is empty the consumer keeps its
previous message, so a stalled control thread degrades freshness and nothing else.
A full queue is counted and self-heals, because the control loop republishes
complete state every tick.

**3. Audio → playback blocks (`BlockChannel`).** A slot's sample memory is only
ever touched by the thread that currently owns it, and ownership moves in one
direction: `Free → Producer → Ready → Consumer → Free`. Producer writes are
published by the release push onto the ready ring and acquired by the consumer's
pop, so `writable(slot)` writes happen-before `readable(ref)` reads. The consumer's
reads are published by the release push onto the free ring and acquired by the
producer's pop, so the consumer's last read happens-before the producer's next
write to that slot. A slot cannot be reused until playback explicitly releases it,
which is what P0-3 lacked. The free ring has exactly one pusher (playback) and one
popper (capture) — the reason `abandon()` retains the slot privately instead of
pushing it (P0-5).

**4. Block occupancy.** `frames_committed_` has one writer (producer),
`frames_taken_` has one writer (consumer), and both are monotone. The consumer
computes the difference; every block it has taken was committed before it could be
popped, and the pop's acquire guarantees the committed total it reads includes
that block, so the subtraction cannot underflow. There is no second counter that
could disagree with the queues about who owns what. Bounded imprecision is
documented at P2-4.

**5. Startup (`StartGate`).** Worker: write `status_`, then
`arrived_.fetch_add(release)`. Supervisor: observe `arrived_.load(acquire) >=
expected` before reading any `status_`. That edge is what makes every worker's
observed scheduling parameters visible to the validating supervisor. The reverse
direction — the release/abort decision — is a `state_.store(release)` read by each
worker's `state_.load(acquire)`, so nothing the supervisor did before releasing
can be invisible to a body that starts running. The release event is deliberately
never consumed, so it stays readable and a late arrival is released immediately
rather than waiting out a timeout.

**6. Shutdown (`Lifecycle`).** `reason_` is compare-exchanged first, then
`stop_.store(release)`; any thread that observes `stopRequested()` via an acquire
load therefore also observes the reason. The stop flag is sticky, so there is no
window in which a thread checks it, misses it, and blocks forever. The eventfd is
never consumed, which makes it a broadcast: every current and future waiter is
released. From a signal handler the whole sequence is async-signal-safe.

**7. Telemetry.** Every counter is written by exactly one thread, read by the
telemetry thread, and ordered against nothing. `memory_order_relaxed` is correct
here for a positive reason, not as a shortcut: there is no happens-before edge to
establish, only eventual visibility of an independent counter. Telemetry values
are never used to make a control decision. `StoreMaxRelaxed` is a relaxed CAS loop
that terminates in one iteration given a single writer.

**Accepted syscalls on the realtime path.** `poll()` and the eventfd `write()` are
syscalls, not userspace-only operations. They are bounded, allocation-free, and
non-blocking (the eventfd is `EFD_NONBLOCK`), and they are the standard
PREEMPT_RT-acceptable way to wait for a device and to wake a thread. This is
stated explicitly because "no syscalls in the audio thread" would be a false
claim about this design.

---

## E. Invariants now mechanically enforced

| # | Invariant | Enforced by |
|---|-----------|-------------|
| 1 | Every thread has an explicit role | `ThreadSpec` name + `ThreadRole` table printed at startup |
| 2 | Final scheduler policy precedes workload | `PTHREAD_EXPLICIT_SCHED` at creation + `StartGate` + `ValidateScheduling`; startup fails if violated |
| 3 | Bounded steady-state RT work | single bounded `poll` per period; per-period runtime and deadline-miss counters |
| 4 | No RT allocation in steady state | all buffers sized and allocated before the gate opens; ASan/UBSan run |
| 5 | No RT-facing message owns dynamic memory | `static_assert(is_trivially_copyable_v<...>)` on `RtSteeringSnapshot`, `BlockRef`, and every `SpscRing<T>` |
| 6 | No `std::mutex` on an RT thread | `StartGate` is atomics + eventfd; no mutex is reachable from either RT body |
| 7 | Every cross-thread queue is bounded | `SpscRing` is fixed-capacity; refusals are counted, never grown |
| 8 | Every audio block has exactly one owner | `BlockChannel` state machine; `ownerOf()` assertions in tests |
| 9 | Queue state cannot disagree with accounting | occupancy derived from two single-writer monotone counters; the separate counter is gone |
| 10 | Slow work cannot run on an RT thread | RT bodies touch only pre-allocated objects and lock-free channels; no provider, parser, or logger is reachable |
| 11 | Telemetry cannot block RT work | RT threads only store to relaxed lock-free atomics; all formatting is on the telemetry thread |
| 12 | Shutdown is singular and deterministic | one `Lifecycle`; fixed join order; devices touched only after every join |
| 13 | No race justified by "the reader retries" | the seqlock is deleted, and its replacement's edge is stated in section D |
| 14 | Scheduler failures are actionable | creation errors surfaced, degradation reported, `require_realtime` turns it into a startup failure |
| 15 | Audio survives a control/network stall | last-snapshot retention; failsafe published when control exits; audio never waits on control |

---

## F. Test results

Verified on x86-64 Linux (WSL2, Ubuntu 22.04, GCC 11.4), non-PREEMPT_RT, ALSA
devices absent.

**Build.** `RelWithDebInfo` with ALSA: 0 errors, 0 warnings. `ctest`: 1/1 passed.

**Sanitizers.** `scripts/run_sanitizer_tests.sh`: ThreadSanitizer clean
(`halt_on_error=1`), then AddressSanitizer + UndefinedBehaviorSanitizer clean
(`detect_leaks=1:abort_on_error=1`, `print_stacktrace=1:halt_on_error=1`). Both
runs exercise the full unit suite including all concurrency tests.

**Race reproducers (negative controls for P0-2).**
`tests/race_repro/old_snapshot_race.cpp` reconstructs the deleted `SnapshotBuffer`
and the original `SteeringSnapshot` and drives them as the runtime did; TSan
reports data races on the snapshot slot inside `SteeringSnapshot::operator=`.
`tests/race_repro/trivially_copyable_seqlock_race.cpp` reconstructs the tip of
PR #23 — trivially copyable payload, upstream `static_assert` present and passing,
seqlock retained — and TSan still reports a data race, on the writer's
`slots_[slot] = value`. The replacement runs the equivalent workload clean.

**Negative control for P0-5.** Reintroducing the producer-side free-queue push
makes `TestAbandonDoesNotRaceConsumerRelease` fail deterministically, so the guard
does not depend on TSan catching a timing window.

**26 new concurrency and lifecycle test cases**, mapped to the brief's 15
requirements:

| Requirement | Test |
|---|---|
| 1 Buffer-pool ownership | `TestBlockOwnershipTransitions`, `TestCommitRejectsOversizedBlock`, `TestAbandonDoesNotRaceConsumerRelease` |
| 2 Producer outruns playback | `TestProducerBlocksWhenConsumerNeverReleases` |
| 3 Playback outruns producer | `TestConsumerStarvesWhenProducerIsSlow` |
| 4 Queue full | `TestRingFullAndEmptyBoundaries`, `TestSteeringChannelFullIsCountedNotFatal` |
| 5 Queue empty | `TestRingFullAndEmptyBoundaries`, `TestConsumerStarvesWhenProducerIsSlow` |
| 6 Shutdown while capture blocked | `TestStopReleasesConcurrentWaiters`, `TestWakeEventReleasesBlockedWaiter` (unit level; device level in section G) |
| 7 Shutdown while playback active | `TestStopIsStickyAndFirstReasonWins`, `TestStopReleasesConcurrentWaiters` |
| 8 Repeated start/stop | `TestStartGateHoldsWorkUntilRelease`, `TestStartGateAbortSkipsWork` (repeated construction) |
| 9 Control publication during audio consumption | `TestConcurrentControlPublication` |
| 10 ThreadSanitizer | whole suite, clean |
| 11 ASan/UBSan | whole suite, clean |
| 12 Scheduler wrapper | `TestSlowThreadIsNeverRealtime`, `TestPriorityValidation`, `TestPrefaultStackIsHarmless` |
| 13 SCHED_FIFO fallback | `TestRealtimeRequestFallbackIsExplicit`, `TestMandatoryRealtimeFailsRatherThanDegrades`, `TestInvalidRealtimePriorityIsRejected` |
| 14 Trivially-copyable assertion | `TestRtMessageHasNoOwningMembers` plus compile-time `static_assert`s |
| 15 Delayed-consumer stress | `TestStressWithDelayedConsumer`, `TestConcurrentOwnershipUnderWrapAround` |

Concurrency tests use atomics and explicit rendezvous rather than sleeps to
establish their conditions. Where a test needs a specific interleaving it forces
it through the channel state, not through timing.

**What these tests do not cover.** No test exercises an ALSA device, so
`prepareWaiting`, `waitForData`, xrun recovery, and the device-teardown ordering
are verified by construction and review only. `TestSlowThreadIsNeverRealtime` and
`TestRealtimeRequestFallbackIsExplicit` observe the *degraded* path when the test
runner lacks `CAP_SYS_NICE`; the granted-FIFO path is only exercised where
privileges allow, which was not the case in this environment.

---

## G. Remaining hardware-only validation

Nothing below can be closed off-target. All of it should run on the Pi with
`realtime.require_realtime: true`.

1. **Confirm the scheduling table.** Start the runtime and check the printed
   table: capture FIFO 80, playback FIFO 78, control and telemetry SCHED_OTHER,
   no thread marked `DEGRADED`. This is the direct on-target refutation of P0-1
   and should be captured as evidence.
2. **Confirm `mlockall` succeeds** (no warning at startup), and record the
   `RLIMIT_MEMLOCK` and capability configuration that makes it succeed.
3. **Soak the pipeline** in both `--mode passthrough` and `--mode beamform` for at
   least an hour and record `cap_deadline_misses`, `cap_period_us_max`,
   `cap_work_us_max`, `pb_write_us_max`, `cap_xruns`, `pb_xruns`,
   `pool_exhausted`, `starvation`, `ready_high_water`, and `ctl_snapshot_age_us`.
4. **Verify the event-driven capture wait on real hardware** — that
   `prepareWaiting()` succeeds on the Pico mic, and that `cap_wait_timeouts` stays
   at zero in steady state. A non-zero timeout count means `poll` is not being
   woken by the device and the fallback path is in use.
5. **Shutdown from a blocked capture.** SIGINT while capture is blocked in `poll`,
   and confirm bounded exit with reason `signal` and no hang.
6. **Shutdown under playback failure.** Force repeated write failures (e.g. remove
   the playback device) and confirm the process stops with reason
   `playback-failure`.
7. **Control-stall behaviour.** Point `odas.endpoint` at an unreachable address and
   confirm that audio continues, `ctl_snapshot_age_us` grows, and the runtime does
   not stall. This is the test that decides whether the network/observation thread
   split in finding K is needed; it was deliberately *not* implemented, because
   introducing it without this measurement would be speculative.
8. **xrun recovery** under deliberate CPU contention.
9. **Repeated start/stop** of the whole process, checking for descriptor or
   device leaks.

---

## H. Scheduling risks and WCET data still required

No WCET measurement exists for this pipeline. The table below is the scheduling
analysis with the WCET column left empty on purpose — filling it with estimates
would be fabrication.

| Thread | Period | Deadline | Blocking | Measured WCET | Policy | Priority |
|--------|--------|----------|----------|---------------|--------|----------|
| capture/DSP | 1.451 ms (64 fr @ 44.1 kHz) | = period | `poll` on ALSA + wake fd | **not measured** | SCHED_FIFO | 80 (provisional) |
| playback | 1.451 ms | = period | `snd_pcm_writei`, `WaitAnyOf` | **not measured** | SCHED_FIFO | 78 (provisional) |
| control | 10 ms | soft, ~`release_hold_ms` (1500 ms) | socket I/O, lifecycle wait | n/a | SCHED_OTHER | — |
| telemetry | 1000 ms | none | lifecycle wait, `std::cout` | n/a | SCHED_OTHER | — |

The instrumentation to fill the WCET column now exists and is per-thread:
`capture_work_max_us` and `capture_period_max_us` for capture,
`playback_write_max_us` and `playback_write_last_us` for playback, plus
`capture_deadline_misses` as the pass/fail signal. Section G.3 is the procedure.

Open scheduling risks:

- **Priority ordering is unjustified by measurement.** Capture above playback is
  reasonable (capture has the hard device deadline and feeds playback) but it is
  an argument, not a result. If measurement shows playback write time dominating,
  the ordering must be revisited.
- **Both realtime threads sit above most kernel threads at 78–80.** On a
  PREEMPT_RT kernel this can starve `ksoftirqd` and the ALSA IRQ threads that the
  audio path itself depends on. The relative priority of the audio threads against
  the target's IRQ threads needs checking on the Pi; it cannot be reasoned about
  from the source.
- **No CPU affinity or `sched_rt_runtime_us` policy is set.** A runaway realtime
  thread is currently bounded only by the kernel's default RT throttling.
- **Single-core contention is untested.** The mechanism behind P0-1 was most
  dangerous on a constrained core count, and the fix should be validated there.
- **`wait_timeout_ms` is 8 nominal periods** (≈12 ms). That is a liveness backstop,
  not a deadline; if it ever fires in steady state, the device is not waking the
  poll and the timing model is wrong. Hence the counter.

---

## I. Final sign-off verdict

### CONDITIONAL APPROVAL — correctness fixed, hardware realtime evidence pending

The four P0 defects in the brief were all confirmed and are fixed, a fifth P0
introduced during remediation was found and fixed before sign-off, and every
cross-thread handoff now has a happens-before argument written down in section D
rather than left to intuition. The data race is gone in the strong sense: the
seqlock is deleted, the replacement's correctness is structural, and the old code
is retained as a reproducer that ThreadSanitizer flags. The scheduler-inheritance
window is gone by construction, because the kernel now applies the final policy at
creation and startup fails if what it granted does not match what the design
requires.

It is not level 3 or 4, for reasons that are about evidence rather than design:

- **No WCET data exists**, so the priority assignment remains provisional. A
  realtime sign-off that cannot state the worst-case execution time of its audio
  thread is not a sign-off.
- **Nothing has run on the target.** All verification was on x86-64 non-PREEMPT_RT
  without the ALSA devices. The event-driven capture wait, xrun recovery, and
  device-teardown ordering are verified by construction and review only.
- **The granted-SCHED_FIFO path is untested**, because the verification
  environment could not grant it. Only the degraded path was exercised.
- **The interaction with the target's kernel threads is unexamined** — priorities
  78–80 sit above IRQ threads the audio path depends on.

Recommended gate to reach level 3: complete section G on the Pi with
`require_realtime: true`, publish the scheduling table and a one-hour soak with
zero `cap_deadline_misses`, and fill in section H's WCET column from
`capture_work_max_us` and `playback_write_max_us`.

Two process notes, both affecting what this sign-off covers.

**PR #23 must not be merged on the strength of `2e59349`.** That commit's message
reads as though the RT snapshot contract is fixed; it is not, and the reproducer in
section F demonstrates it. Merging it would close these findings on the tracker
while leaving a data race in the control-to-audio path and the entire
scheduler-inheritance defect untouched. The assessment box in section B is the
part of this report to read before approving that PR.

**The local `audit/remediation` branch was not reviewed.** It carries roughly 50
uncommitted modified files and is not on the remote. If it overlaps these areas it
has not been looked at here and will need reconciling before anything is merged.
