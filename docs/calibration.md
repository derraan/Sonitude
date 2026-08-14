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

### Current estimator assumptions

`sonitude_calibration_estimate` currently computes gain as **relative trim against the reference channel (`channels[0]`)**:

- `gain_linear[channel] = reference_rms / channel_rms`
- `gain_linear[reference] = 1.0`

This requires a controlled common excitation across microphones (same source and stable level) so that RMS ratios represent capsule-response differences rather than source-position changes.

Every estimator invocation must provide `--min-correlation <0..1>`. The estimator rejects each
non-reference channel whose mean-removed normalized peak correlation is below that threshold,
before it creates, backs up, or replaces the YAML output. Hardware acceptance must use the
project-approved threshold selected through hardware characterization.

Synthetic/testing estimation must be explicit and separately named:

```bash
./build/sonitude_calibration_estimate \
  --input build/ci_capture.synthetic.wav \
  --output build/ci_estimate.yaml \
  --min-correlation 0 \
  --synthetic
```

The zero threshold above is only an execution-path test value. It is not a hardware acceptance
threshold and must not be used to approve calibration evidence.

If hardware acceptance needs an absolute SPL calibration protocol, define that protocol in the hardware runbook before promoting these estimates to production calibration artifacts.

## Limits and assumptions

- Geometry in `config/geometry_soundbubble_initial.yaml` is provisional planar data.
- Final production calibration must use measured 3D acoustic port coordinates.
