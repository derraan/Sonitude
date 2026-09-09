# Calibration

Calibration is split into two layers:

1. **Offline compiler** (`tools/calibration/`) that derives calibration artifacts from measurement WAVs.
2. **Runtime application** (`src/dsp/`) that applies those artifacts in real time.

The PySide6 test bench **Calibration** tab (`testbench/app/ui/calibration_tab.py`)
is a GUI front-end for the same compiler: import measurement WAVs (multi-azimuth
table with bypass checkboxes), optional REW `.mdat` metadata, set the compiler
knobs, write A–E YAML + reports, then apply a variant to Recorded / Real-Time via
a runtime-config overlay. It does not reimplement DSP.

This document is authoritative for current behavior.

## Runtime chain (implemented)

Per microphone, `CalibrationApplier` runs:

1. polarity (`+1` / `-1`)
2. DC offset subtraction
3. scalar gain
4. one-pole high-pass (`calibration_dc_block_hz`, default 20 Hz)

`delay_samples` is **not** applied in `CalibrationApplier`.
It is consumed by `MvdrBeamformer` as steering phase in
`computeRelativeDelays()`:

```text
relative_delay_i = (calibration_delay_i + geometric_delay_i) - calibration_delay_ref
```

## Calibration YAML contract

```yaml
sample_rate_hz: 44100
channels:
  - id: M0_left_ear
    polarity: 1
    gain_linear: 1.0
    delay_samples: 0.0
    dc_offset: 0.0
    eq:
      enabled: false
      sections:
        - {type: PK, freq_hz: 2000.0, gain_db: 2.0, q: 1.2}
```

Validation rules:

- exactly six channels, one per geometry microphone ID
- `polarity` in `{+1, -1}`
- `gain_linear` in `(0, 8]`
- `|delay_samples| <= 256`
- per-channel EQ supports at most 24 sections
- section `type` in `PK | LS | HS | LP | HP`
- section `freq_hz` in `(0, sample_rate/2)`
- section `q` in `(0, 20]`
- section `|gain_db| <= 24`

## Runtime common EQ (implemented)

Runtime also supports one **common** mono EQ stage after beamforming and before limiting:

```yaml
common_eq:
  enabled: false
  sections: []
```

This is for voicing; it does not replace per-mic calibration.

## Offline compiler workflow

Entry point:

```text
python -m tools.calibration --array-wav <6ch_0deg.wav> --element-wavs <M0.wav> ... <M5.wav> --geometry <geometry.yaml> --out-dir <dir>
```

Inputs:

- one or more simultaneous 6-channel array sweeps at 1 m (primary azimuth feeds
  runtime YAML; default characterization grid is
  `0, ±30, ±60, ±90, ±120, ±140, ±160, ±180` deg — GUI rows are bypassable)
- six same-position element sweeps
- geometry YAML (`config/geometry_soundbubble_initial.yaml`)
- optional REW filter export (`filter-wholearray-formatted.txt`)
- optional REW `.mdat` (metadata / delay / level notes; not a full IR deserializer)
- **stimulus WAV** for absolute TOF (`--stimulus-wav`, delay mode `absolute_tof`)

Delay modes:

- `absolute_tof` (default): GCC-PHAT each mic against the played stimulus → absolute
  TOF samples/ms; relative lag is `tof_i - tof_ref`; then geometry is removed for
  runtime `delay_samples`
- `relative`: legacy mic-vs-mic GCC-PHAT (no stimulus)

CLI extras:

```text
--extra-array-angle <azimuth_deg> <6ch.wav>   # repeatable
--stimulus-wav <played_stimulus.wav>
--delay-mode absolute_tof|relative
--max-tof-samples <N>   # 0 = auto (distance/c + 100 ms)
```

## REW `.mdat` ingest

`tools/calibration/mdat_parse.py` reads REW Measurement Data File V2 and extracts
embedded note strings:

- per-measurement Delay (ms) and distance note
- timing / measurement peak levels (dBFS)
- FR summary cards (frequency span, SPL span, date)
- source WAV names and array channel indices

It does **not** deserialize Java IR/FR sample arrays (that still needs REW's
classes or the REW HTTP API). Exported WAVs remain authoritative for GCC-PHAT.
Outputs:

- `calibration_<tag>_A_baseline.yaml`
- `calibration_<tag>_B_delay.yaml`
- `calibration_<tag>_C_polarity.yaml`
- `calibration_<tag>_D_delay_polarity.yaml`
- `calibration_<tag>_E_full.yaml`
- `calibration_report.json`
- `calibration_report.md`

Estimation details:

- delay: banded GCC-PHAT (`300-3k`, `3k-8k`, `300-8k`) with parabolic lag refine
- polarity: signed correlation at selected lag, with ambiguity threshold
- gain: 300-8000 Hz band RMS ratio vs reference mic
- delays are emitted in runtime sign convention and adjusted for geometry term

## REW filter import policy

`tools/calibration/rew_filters.py` parses `filter-wholearray-formatted.txt` and emits:

- verbatim section list
- guarded section list (caps on Q, boost, and minimum frequency)
- dropped/tamed reasons
- worst-case cumulative boost estimate

The guarded list is intended for runtime use.

## Notes

- Exported IR peaks from REW are commonly normalized/positioned and are not timing-authoritative for delay estimation.
- Milestone gate evidence still requires hardware capture validation on target Pi.
