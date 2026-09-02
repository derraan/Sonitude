# Raspberry Pi Hardware Test Record — 2026-08-15

## Provenance

- Repository: `derraan/Sonitude`
- Draft PR: [#28](https://github.com/derraan/Sonitude/pull/28)
- Branch: `integration/release-candidate-2026-08-14`
- Tested commit: `cb8b52263f4a867cb93521d245e7ad6487f46774`
- Snapshot SHA-256: `cd54733667a0925ba4e74da91585e7cc356110a6433cc2a9d9fe5c9507377412`

## Exact tested local deltas

Pi local tested runtime values (not tracked in Git at run time):

- capture ALSA ID: `hw:active,0`
- playback ALSA ID: `hw:X1,0`
- `active_channel_map: [5,4,3,2,1,0]`
- `geometry_path: geometry_soundbubble_initial.yaml`
- `calibration_path: calibration_example.yaml` (unity gains)
- ODAS disabled
- suppression disabled

An untracked `calibration_gain_test.yaml` containing 7x/8x gains was not active.

## Scheduling summary (short passthrough gate)

| Thread role | Requested | Observed |
| --- | --- | --- |
| capture/DSP | `SCHED_FIFO/80` | `SCHED_FIFO/80` |
| playback | `SCHED_FIFO/78` | `SCHED_FIFO/78` |
| telemetry | `SCHED_OTHER/0` | `SCHED_OTHER/0` |

No control thread exists in passthrough mode.

Memory locking passed by startup invariant: `require_memory_lock: true` was set,
and runtime proceeded to worker startup.

## Telemetry summary (PID 1240, 293 samples)

| Metric | Observed |
| --- | --- |
| runtime duration | about 293.27 s (`cap_frames=12933312` at 44.1 kHz) |
| final `pb_frames` | `12934146` |
| `cap_xruns` / `pb_xruns` | `0 / 0` |
| `pb_write_fail` | `0` |
| `cap_wait_timeouts` / `cap_wait_errors` | `0 / 0` |
| `cap_overflow_refusals` | `0` |
| `block_commit_fail` | `0` |
| `pool_exhausted` | `0` |
| `cap_deadline_misses` | `0` |
| `ready_high_water` | `2` |
| `free_slots` | `14..16` |
| `occupancy` | `93..164` |
| `asrc_ppm` | `-520..+4700` |
| `cap_period_us_max` | `2051` |
| `cap_work_us_max` | `20` |
| `pb_write_us_max` | `1826` |
| legacy `starvation` | `196113` |

`+4700` ppm occurred on startup and remained within the configured `+5000` ppm
bound. The legacy `starvation` counter represented empty software ready-queue
checks before waiting and was not an ALSA XRUN counter.

## Geometry observation

With the original geometry, a physical front source was strongest when steering
was set to `+90°`. This confirmed the geometry-axis mapping was rotated 90°
relative to the runtime steering convention (`0° == +X`).

## Pass / pending matrix

| Gate | Status |
| --- | --- |
| fail-closed startup on tested Pi config | pass |
| passthrough scheduling policy check | pass |
| short passthrough stability check | pass |
| one-hour passthrough soak | pending |
| one-hour beamform soak | pending |
| corrected-geometry static beam check | pending |
| scripted beamform steering check | pending |
| live ODAS loss/recovery check | pending |
| current six-active-channel calibration capture | pending |
| forced playback failure gate | pending |
| blocked-capture shutdown gate | pending |
| latency loopback measurement | pending |

## Remaining acceptance sequence

1. Run one-hour passthrough soak with PID-filtered telemetry and final summary.
2. Run one-hour beamform soak (adds control thread at `SCHED_OTHER/0`).
3. Execute corrected-geometry static beam checks (front/right/left/rear).
4. Run scripted mock steering transitions in beamform mode.
5. Run live ODAS disconnect/recovery checks.
6. Capture current six-active-channel calibration evidence and thresholded estimate.
7. Execute forced playback failure and blocked-capture shutdown gates.
8. Produce latency loopback median/p95/p99 artifact.

## Raw evidence location

The full raw journal stream (including all 293 telemetry lines) was supplied
outside Git. This repository stores only bounded summaries and provenance.
Limitations: replay/forensic detail depends on access to the external archived
journal artifact.
