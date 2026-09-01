# Calibration

Milestone 3 provides evidence-driven per-channel calibration for the six-microphone Sonitude host DSP. Calibration data is estimated offline, validated against geometry and runtime sample rate, and consumed at runtime by `CalibrationApplier` (time domain) and `MvdrBeamformer` (steering delay phase).

Status: **IMPLEMENTED (offline/synthetic)** · **HARDWARE-EVIDENCE-PENDING**

## Calibration model

Per microphone `m`, the time-domain path applies:

```text
x_cal,m[n] = polarity[m] × gain[m] × (x_m[n] - dc_offset[m])
```

followed by the existing DC blocker (`calibration_dc_block_hz`, default 20 Hz).

The MVDR steering vector applies fractional calibration delay as phase:

```text
d_m(k, θ) = exp(j 2π k Δ_m(θ) / N)
Δ_m(θ) = geometry_delay_m(θ) + calibration_delay_m - reference_delay
```

`delay_samples` is **not** applied in `CalibrationApplier` to avoid double correction.

## Reference microphone convention

- Default reference: geometry channel index **2** (`M2_left_top`), matching `steering.reference_mic_index` in `config/default.yaml`.
- Gain normalization: each channel is scaled so its RMS matches the reference microphone RMS in the calibration signal region.
- Delay sign: positive `delay_samples` advances that channel relative to the reference in steering phase. GCC-PHAT lag is stored as the negative of measured lag so late channels receive compensating phase.

## Artifact schema (v2)

```yaml
schema_version: 2
sample_rate_hz: 44100
reference:
  microphone_id: M2_left_top
identity:
  geometry_id: soundbubble_vertical_v1
  created_utc: 2026-09-02T00:00:00Z
capture:
  sample_rate_hz: 44100
  channel_count: 6
channels:
  - id: M0_left_ear
    polarity: 1
    gain_linear: 1.0
    delay_samples: 0.0
    dc_offset: 0.0
quality:
  valid: false
  hardware_evidence: false
  warnings: []
```

Schema v1 files (no `schema_version`) remain loadable.

`quality.valid` is only true when estimation status is PASS **and** `hardware_evidence` was asserted at estimation time. Parsing a valid YAML file does not imply a valid calibration.

## Capture procedure

`sonitude_calibration_capture` writes a six-channel WAV with:

1. **Silence region** (~0.5 s): DC offset estimation, ambient-noise check.
2. **Common-source region** (~2 s): multi-tone signal for gain, polarity, and GCC-PHAT delay estimation.

Channel order matches `config/geometry_soundbubble_initial.yaml` (USB indices 0..5).

```bash
./build/sonitude_calibration_capture --output build/calibration_capture.wav
./build/sonitude_calibration_estimate build/calibration_capture.wav build/calibration_estimate.yaml \
  --geometry config/geometry_soundbubble_initial.yaml \
  --reference-index 2 \
  --report build/calibration_report.txt
```

For deterministic software validation:

```bash
./build/sonitude_calibration_capture --output build/calibration_capture_synth.wav --synthetic-test
./build/sonitude_calibration_estimate build/calibration_capture_synth.wav build/calibration_estimate_synth.yaml \
  --geometry config/geometry_soundbubble_initial.yaml --report build/calibration_report_synth.txt
```

### Hardware capture checklist (not yet executed)

| Step | Purpose |
| --- | --- |
| Channel activity/order | Confirm USB map 0..5 |
| Silence capture | DC and ambient noise |
| Common-source capture | Gain and relative delay |
| Repeat capture | Repeatability |
| Front/left/right anchors | Geometry + calibration phase validation |
| MVDR replay before/after | End-to-end steering closure |

Record for each run: date, hardware revision, geometry file, sample rate, capture file, artifact, analysis command, result, operator notes.

## Estimator outputs

The companion report includes per-channel DC, gain, relative gain dB, delay (samples and µs), polarity (or UNRESOLVED), correlation peak, delay confidence, RMS, clipping flag, and warnings.

Statuses: `PASS`, `WARNING`, `UNRESOLVED`, `INVALID`.

## Validation guardrails

Loader rejects: sample-rate mismatch, wrong channel count, duplicate/unknown IDs, invalid polarity, non-finite values, out-of-range gain (0, 8], delay magnitude > 256 samples (engineering guardrail), missing reference microphone.

## Spectral residual correction

**NOT ESTABLISHED — HARDWARE DATA REQUIRED.** Scalar gain + fractional delay remain the runtime model. Frequency-dependent complex correction will only be added if measured sweeps show material residual mismatch after scalar calibration.

## Future firmware serialization boundary

```text
Host YAML + report
    → firmware calibration packer (future repo)
    → versioned binary artifact (schema, sample rate, topology, gain, delay, optional Q15 residual, CRC)
    → future MCU loader
```

Dual-slot Flash/CRC semantics belong to the embedded integration boundary and are not implemented in this repository.

## Limits and assumptions

- Geometry in `config/geometry_soundbubble_initial.yaml` is provisional planar data (hardware-unverified).
- Measurement FFT (offline GCC-PHAT) is separate from runtime MVDR FFT (128/32).
- Do not mark M3 `done` until hardware gate evidence is recorded in `docs/milestones.md`.
