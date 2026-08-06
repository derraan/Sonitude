# Sonitude Architecture (Milestone 0 Baseline)

This document defines the target architecture and ownership constraints. Milestone 0 provides only scaffold and configuration validation; real-time dataflow implementation begins in Milestone 1.

## Target pipeline layers

```text
Pico USB ALSA capture (6 active channels)
  -> capture timestamp / sequence accounting
  -> channel calibration (polarity, gain, delay, DC blocker)
  -> fan out to:
     A) real-time audio path
     B) non-critical spatial control path
```

Audio path target order:

```text
steering snapshot -> delay-and-sum beamformer -> conservative suppression policy
-> limiter -> mono-to-stereo -> ASRC/drift control -> ALSA playback
```

Control path target order:

```text
ODAS/VAD/localization -> source confidence and zone selection
-> conversation state machine -> steering update -> atomic handoff
```

## Thread model (target)

- **Capture worker thread (RT):** ALSA capture, sequence accounting, ring publication.
- **Audio render thread (RT):** beamforming, suppression policy, limiting, ASRC feed.
- **Playback worker thread (RT):** ALSA playback, underrun recovery telemetry.
- **Control thread (non-RT):** ODAS client, source association, state machine.
- **Telemetry thread (non-RT):** aggregate counters, structured output, diagnostics.

## Data ownership rules

- Real-time threads are allocation-free after startup.
- No mutex acquisition on real-time threads.
- No file/network I/O, console output, or blocking IPC waits on real-time threads.
- Control-to-audio handoff uses atomics or immutable double-buffer snapshots.
- Ring buffers are SPSC lock-free for producer/consumer pairs.

## Failure behavior (target)

- ODAS/control failure must not interrupt audio.
- On control confidence loss, steering ramps to safe forward/ambient mode.
- ASRC drift controller must prevent long-run playback buffer runaway between independent capture/playback clocks.

## Known Milestone 0 limitations

- No ALSA capture/playback path yet.
- No calibration processing yet.
- No beamforming/suppression runtime yet.
- No ODAS adapter yet.
- No latency instrumentation yet.
