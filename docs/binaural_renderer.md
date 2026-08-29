# Binaural Renderer Implementation Notes

## Implemented files

- `src/dsp/binaural_renderer.hpp`
- `src/dsp/binaural_renderer.cpp`
- `src/dsp/hrtf_table.hpp`
- `src/dsp/hrtf_table.cpp`

## Coordinate system

Authoritative head-centric frame:

- azimuth `0 deg` points to front (`+Y`)
- positive azimuth rotates clockwise toward listener-right (`+X`)
- elevation `+` points upward (`+Z`)
- azimuth wrap range is `(-180, 180]`

The helper contract is defined in `src/spatial/head_frame.hpp`.

## ITD/ILD model

Implemented model:

- ITD uses Woodworth spherical-head approximation:
  `tau = (a / c) * (lambda + sin(lambda))`
- `a` is `head_radius_m` from config.
- `c` is fixed to `343 m/s` in this reference implementation.
- `lambda` is the lateral angle from the shared head frame.
- ILD is a broadband gain law capped by `max_ild_db`.

This backend is deterministic and portable. It does not claim personalised HRTF accuracy.

## Transition method

Direction changes use dual-path crossfade:

- keep current path state
- render pending path in parallel
- linearly crossfade over `transition_ms`
- swap active path at fade completion
- **identical `setDirection()` calls are ignored** so a live tool that
  restates the same azimuth every block cannot restart the fade

`sonitude_stream_process` currently calls `setDirection()` on every block.
The renderer, not the caller, is responsible for treating a repeated target
as a no-op. The same rule applies to `DelaySumBeamformer::setTarget()`.

This matches the click-safe strategy already used by the beamformer.

`transition_ms` tuning remains `TODO(TBD_FROM_MEASUREMENT)`.

## HRTF backends

- `CompactHrtf` and `FullHrtfReference` are both data-driven from `HrtfTable`.
- Compact tables (`compact_16/32/64.shrf`) are **experimental raw-HRIR prefix
  candidates**: the preparation tool copies the first N samples of each SADIE
  II D2 HRIR and writes explicit left/right delays as zero. They are **not**
  a measured compact approximation and **not** a selected Pico/STM32
  representation.
- If a table is unavailable, configuration fails with a clear error.
- No synthetic fallback is silently selected.
- Runtime renderer API remains independent from SOFA parsing and filesystem I/O.

## Data provenance

Current profile directory:

- `data/hrtf/generic_sadie2_d2/`

Source summary:

- SADIE II v2-2 dataset (University of York)
- subject D2 (KEMAR)
- Apache-2.0 license
- azimuth converted into Sonitude frame (`sonitude_az = normalize(-sadie_az)`)

Generated artifacts:

- `compact_16.shrf` — experimental 16-tap HRIR prefix candidate
- `compact_32.shrf` — experimental 32-tap HRIR prefix candidate (YAML default)
- `compact_64.shrf` — experimental 64-tap HRIR prefix candidate
- `reference.shrf` — 256-tap full-HRIR reference
- `provenance.json`
- `LICENSE`
- `NOTICE`

`tools/hrtf/prepare_hrtf.py --target-rate` must equal the SOFA sample rate.
Resampling is not implemented; a mismatched rate is rejected rather than
relabelling coefficients.

## Latency and memory

- ITD/ILD uses a non-negative base delay plus per-ear fractional delays.
- `algorithmicLatencySamples()` is **first-arrival latency**: the sample
  index of the first output that can contain a time-0 impulse, including the
  8-tap Hann interpolator's first non-zero tap (`kFirstArrivalTapOffset = 1`)
  and the first non-zero FIR tap. It is not group delay, not IR tail
  (`taps-1`), and not host buffering. Unit tests compare it to an impulse.
- `stateBytes()` sums four fractional-delay buffers, four FIR ring buffers,
  and the working FIR coefficient copies. It is renderer heap RAM, not a
  Pico/STM32 measurement.

## Configuration

`config/default.yaml` has an optional `binaural:` section. Paths in
`binaural.profile.table_path` are resolved relative to the YAML file, same as
`geometry_path`. The default compact table is therefore:

```text
../data/hrtf/generic_sadie2_d2/compact_32.shrf
```

`full_hrtf_reference` loads `reference.shrf` from the same directory.

Unknown backend names fail configuration. There is no silent backend fallback
inside `BinauralRenderer`. Stream/replay tools may still emit L=R of directional
mono and set protocol flag `BINAURAL_UNAVAILABLE` when an HRTF table cannot be
loaded at runtime (that is the protocol-defined tool fallback, not a renderer
fallback).

## Tools and protocol v2

The renderer is wired into the portable CLI tools. `sonitude_realtime`
(`src/main.cpp`) is not yet on this path; `--mode beamform` still duplicates
mono to both playback channels.

### `sonitude_wav_replay`

- `--output` stays **1-channel** processed mono (post-suppression, post-limiter).
- `--output-binaural <stereo.wav>` writes the renderer output.
- `--binaural-backend` selects `mono_reference`, `itd_ild`, `compact_hrtf`, or
  `full_hrtf_reference`.
- `--binaural-follow-steering` / `--binaural-fixed-direction` override YAML
  `binaural.direction.follow_steering`. With a fixed direction, `--binaural-azimuth`
  and `--binaural-elevation` override YAML angles.
- `--output-mono-pre-binaural` is an alias for the pre-limiter suppressed tap.
- `--capabilities` prints protocol JSON (see below).
- `--suppression auto|on|off` matches the test-bench contract.

### `sonitude_stream_process`

Framing is protocol v2 only. Python and C++ must stay in lockstep:

- `testbench/app/processing/protocol.py`
- `src/tools/stream_process.cpp`

| | Input | Output |
| --- | --- | --- |
| Magic | `0x32424253` (`SBB2`) | `0x324F4253` (`SBO2`) |
| Header | 48 bytes | 24 bytes |
| PCM | 6-channel float32 | 2-channel float32 |

Input flags: bit0 suppression focus, bit1 binaural enabled, bit2 follow steering.
Output flags: bit0 suppression applied, bit1 binaural applied, bit2 binaural
unavailable, bit3 mono reference.

Backend bytes: `0` none, `1` mono_reference, `2` itd_ild, `3` compact_hrtf,
`4` full_hrtf_reference. `none` with binaural enabled is treated as
`mono_reference`.

When binaural is disabled, the tool still emits stereo as L=R of limited
directional mono (the live test-bench path always consumes stereo).

### `--capabilities`

Both tools print a one-line JSON object on stdout, including
`protocol_version: 2` and the implemented binaural backends. The GUI uses this
to populate backend lists (`testbench/app/processing/capabilities.py`).
`BinauralControls` defaults to `compact_hrtf` when advertised, passes
follow/fixed direction into wav_replay CLI flags, and (on the Recorded tab,
Live DSP) into each `stream_process` block. `--output` stays mono; listen to
the Binaural stage or the live preview.

## Known limitations

- ITD/ILD is a generic spherical-head model only.
- No perceptual equivalence claim vs full HRTF.
- Compact 16/32/64 tables are prefix truncations, not validated compact HRTFs.
- No hardware realtime claim for Pico 2W or STM32 yet.
- HRTF direction lookup currently uses nearest-neighbour selection.
  `TODO(TBD_FROM_MEASUREMENT): evaluate interpolation strategy.`
