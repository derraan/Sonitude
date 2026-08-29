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

This matches the click-safe strategy already used by the beamformer.

`transition_ms` tuning remains `TODO(TBD_FROM_MEASUREMENT)`.

## HRTF backends

- `CompactHrtf` and `FullHrtfReference` are both data-driven from `HrtfTable`.
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

- `compact_16.shrf`
- `compact_32.shrf`
- `compact_64.shrf`
- `reference.shrf`
- `provenance.json`
- `LICENSE`
- `NOTICE`

## Latency and memory

- ITD/ILD uses a non-negative base delay plus per-ear fractional delays.
- Fixed algorithmic latency is queryable via `algorithmicLatencySamples()`.
- Estimated state RAM and coefficient storage are queryable via
  `stateBytes()` and `coefficientBytes()`.

## Known limitations

- ITD/ILD is a generic spherical-head model only.
- No perceptual equivalence claim vs full HRTF.
- No hardware realtime claim for Pico 2W or STM32 yet.
- HRTF direction lookup currently uses nearest-neighbour selection.
  `TODO(TBD_FROM_MEASUREMENT): evaluate interpolation strategy.`
