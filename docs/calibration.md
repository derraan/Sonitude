# Calibration (Planned)

Milestone 0 defines calibration file schema and validation boundaries. Runtime calibration processing starts in Milestone 3.

## Calibration file contract

```yaml
sample_rate_hz: 44100
channels:
  - id: M0_upper_inner_left
    polarity: 1
    gain_linear: 1.0
    delay_samples: 0.0
```

Per-channel parameters:

- `polarity`: `1` or `-1`.
- `gain_linear`: fixed scalar gain.
- `delay_samples`: integer + fractional delay relative to reference mic.

## Processing order (target)

1. Polarity correction
2. Gain trim
3. Integer/fractional delay compensation
4. DC blocker/high-pass conditioning

## Offline calibration tooling (Milestone 3 target)

- Input: six-channel WAV recordings from impulse/swept-sine tests.
- Output:
  - estimated polarity/gain/delay offsets
  - human-readable report
  - generated YAML calibration candidate
- Safety requirement: do not overwrite existing calibration without explicit backup/confirmation.

## Limits and assumptions

- Geometry in `config/geometry_soundbubble_initial.yaml` is provisional planar data.
- Final production calibration must use measured 3D acoustic port coordinates.
