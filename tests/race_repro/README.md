# Data-race reproducers for the removed snapshot handoff

These files are standalone reproducers kept as evidence for the thread-safety
audit. They are **not** part of the build; they reconstruct code from other
commits so the defect can be demonstrated rather than only argued.

Build and run either of them under ThreadSanitizer:

```bash
g++ -std=c++20 -fsanitize=thread -O1 -g \
    tests/race_repro/<file>.cpp -o /tmp/repro
setarch --addr-no-randomize /tmp/repro
```

## `old_snapshot_race.cpp`

The deleted `src/rt/param_snapshot.hpp` seqlock together with the original
`SteeringSnapshot`, including its `std::string zone_name`, driven the way the
runtime drove it: one control-style writer publishing, one audio-style reader
acquiring. ThreadSanitizer reports write/read races on the snapshot slot inside
`SteeringSnapshot::operator=`. With the string present the consequences are worse
than tearing: a racing copy of a `std::string` can double-free or read a dangling
pointer.

## `trivially_copyable_seqlock_race.cpp`

The state of the handoff at the tip of PR #23 (commit `2e59349`). That commit
removed `std::string zone_name` and added
`static_assert(std::is_trivially_copyable_v<SteeringSnapshot>)`, but kept the
seqlock as the control-to-audio mechanism.

ThreadSanitizer still reports a data race, on the writer's non-atomic
`slots_[slot] = value`. This is the point worth keeping: making the payload
trivially copyable constrains the *message* and does nothing about the *handoff*.
The writer still assigns to a non-atomic object while the reader copies the same
object, nothing orders those accesses, and a data race is undefined behaviour
regardless of how simple the type is. The sequence re-check discards torn results
but cannot retroactively make the conflicting accesses well-defined.

## The replacement

`src/control/steering_channel.hpp` over `src/rt/spsc_ring.hpp`. A slot is only
ever touched by one thread at a time, and the ring's release/acquire pair on the
index supplies the happens-before edge for the payload, so there is no
conflicting access to order. The equivalent workload runs clean under
ThreadSanitizer; see `TestConcurrentControlPublication` in
`tests/unit/thread_safety_tests.cpp`.
