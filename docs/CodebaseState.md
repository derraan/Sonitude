# CodebaseState

Living snapshot for Sonitude scope and implementation status. `docs/milestones.md` remains authoritative for gate evidence.

Last updated: 2026-08-12.

## Global scope (v1 baseline)

- Direct ALSA capture/playback on Pi 5 host path.
- ODAS control metadata only (no ODAS PCM in audio path).
- Time-domain delay-and-sum beamforming plus one conservative suppressor.
- No MVDR/GSS/neural processing in baseline milestones.
- No end-to-end latency claims before M8 measurement evidence.

## Audio pipeline status (Stages 2-4 focus)

```text
capture -> calibration -> beamformer -> suppressor -> limiter -> mono-to-stereo -> ASRC -> playback
```

| Stage | Milestone | Current status | Notes |
| --- | --- | --- | --- |
| Stage 2 | M3 calibration | in_progress | `CalibrationApplier` live in runtime; HW sweep evidence still pending |
| Stage 3 | M4 beamformer | in_progress | Fractional delay-and-sum, steering ramp, and WAV replay implemented |
| Stage 4 | M7 suppression/limiter | in_progress | Conservative suppressor + peak limiter integrated in beamform mode |

## Implemented vs planned blocks

| Block | Status |
| --- | --- |
| ALSA capture/playback workers | Implemented |
| Channel extraction + float conversion | Implemented |
| Calibration apply (polarity/DC/gain/HP) | Implemented |
| Calibration delay closure in beamformer | Implemented |
| Delay-and-sum beamformer | Implemented |
| Suppression v1 (conservative, floor-clamped) | Implemented |
| Limiter v1 (peak limiter) | Implemented |
| ASRC PI + stereo resampler | Implemented |
| ODAS control adapter + mock provider | Implemented |
| Conversation state machine | Implemented |
| M8 latency instrumentation evidence | Pending hardware run |

## Validation artifacts

- Unit tests for beamformer, suppressor, and limiter are in `tests/unit/`.
- Offline beamforming render tool: `build/sonitude_wav_replay`.
- openMHA comparison workflow and evidence template:
  - `scripts/openmha_golden_render.sh`
  - `tests/integration/openmha_m4_validation.md`

## Scope guardrails (active)

- SCOPE-1: no JACK/PipeWire/PulseAudio in critical path.
- SCOPE-2: ODAS remains control-only.
- SCOPE-3: no MVDR/LCMV/GSS/neural in v1 baseline.
- SCOPE-4: no latency claims without M8 measurement.
- SCOPE-6: no milestone marked done without evidence.
- SCOPE-7: keep openMHA and firmware code out-of-tree.
