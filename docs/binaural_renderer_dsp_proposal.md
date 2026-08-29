# Sonitude Binaural Renderer DSP Proposal (Repo Copy)

This file is the in-repo implementation target for the binaural DSP work.

## Scope

- Implement a portable C++ renderer in `src/dsp/`.
- No Python, Qt, ALSA, or GUI dependencies in renderer code.
- Block-oriented API with deterministic `configure`/`reset`/`setDirection`/`process`.
- No heap allocation in the realtime `process` loop after initialization.

## Phase 1 backends

- `MonoReference`: identity `L = mono`, `R = mono`.
- `ItdIld`: fractional-delay plus broadband ILD model.

## Architectural backends

- `CompactHrtf`: data-driven compact FIR backend.
- `FullHrtfReference`: data-driven longer FIR reference backend.

Unsupported or unavailable backends must fail clearly at configuration time.

## Direction convention

One authoritative head frame is used across beamformer and renderer:

- azimuth `0 deg` = front (`+Y`)
- positive azimuth = clockwise (toward listener-right, `+X`)
- elevation positive = up (`+Z`)

## Required chain order

`Calibration -> Beamformer -> Directional/Omni blend -> Suppression -> BinauralRenderer -> StereoPeakLimiter`

## Claim control

Do not claim:

- personalised HRTF accuracy
- validated externalization/elevation
- compact/full perceptual equivalence
- verified Pico 2W or STM32 realtime capability
- optimal FIR length

Use measurement-gated TODO markers where evidence is required:

- `TODO(TBD_FROM_MEASUREMENT)`
- `TODO(TBD_FROM_HARDWARE_EVIDENCE)`
