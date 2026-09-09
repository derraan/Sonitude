# Calibration

Milestone 3 provides evidence-driven per-channel calibration for the six-microphone Sonitude host DSP. Calibration data is estimated offline, validated against geometry and runtime sample rate, and consumed at runtime by `CalibrationApplier` (time domain) and `MvdrBeamformer` (steering delay phase).

Status: **IMPLEMENTED (offline/synthetic)** · **HARDWARE-EVIDENCE-PENDING**

Calibration is split into two layers:

1. **Offline compiler** (`tools/calibration/`) that derives calibration artifacts from measurement WAVs.
2. **Runtime application** (`src/dsp/`) that applies those artifacts in real time.

The PySide6 test bench **Calibration** tab (`testbench/app/ui/calibration_tab.py`)
is a GUI front-end for the same compiler: import measurement WAVs (multi-azimuth
table with bypass checkboxes), optional REW `.mdat` metadata, set the compiler
knobs, write A–E YAML + reports, then apply a variant to Recorded / Real-Time via
a runtime-config overlay. It does not reimplement DSP. The **Upload** tab commits
a selected variant plus DSP knobs into `config/default.yaml`.

## Calibration model

Per microphone `m`, the time-domain path applies:

```text
x_cal,m[n] = polarity[m] × gain[m] × (x_m[n] - dc_offset[m])
```

followed by the existing DC blocker (`calibration_dc_block_hz`, default 20 Hz)
and optional per-mic EQ (`eq.enabled` + biquad sections).

The MVDR steering vector applies fractional calibration delay as phase:

```text
d_m(k, θ) = exp(j 2π k Δ_m(θ) / N)
Δ_m(θ) = geometry_delay_m(θ) + calibration_delay_m - reference_delay
```

`delay_samples` is **not** applied in `CalibrationApplier` to avoid double correction.

## Reference microphone convention

- Default reference: geometry channel index **2** (`M2_left_top`), matching `steering.reference_mic_index` in `config/default.yaml`.
- Gain normalization: each channel is scaled so its RMS matches the reference microphone RMS in the calibration signal region.
- Delay sign: positive `delay_samples` advances that channel relative to the reference in **legacy geometric** steering phase. The channel diagnostic estimates a normalized time-domain correlation lag (not GCC-PHAT spectral weighting) and stores the negative of that lag so late channels receive compensating phase.
- Production measured steering does **not** use `delay_samples`. Complex RTFs compiled from synchronized IRs already contain delay in their phase.

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
    eq:
      enabled: false
      sections:
        - {type: PK, freq_hz: 2000.0, gain_db: 2.0, q: 1.2}
quality:
  valid: false
  hardware_evidence: false
  warnings: []
```

Schema v1 files (no `schema_version`) remain loadable.

`quality.valid` is only true when estimation status is PASS **and** `hardware_evidence` was asserted at estimation time. Parsing a valid YAML file does not imply a valid calibration.

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

## Capture procedure

`sonitude_calibration_capture` writes a six-channel WAV with:

1. **Silence region** (~0.5 s): DC offset estimation, ambient-noise check.
2. **Common-source region** (~2 s): multi-tone signal for gain, polarity, and normalized-correlation delay estimation.

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

## Physical IR compiler (SMV3)

`tools/calibration/compile_array.py` supports measured IR import via a JSON/YAML
manifest and emits an SMV3 profile (`.bin`, `.npz`, `.csv`, `.report.json`)
for `spatial.backend: fixed_measured`.

- This tool assumes synchronized per-direction IR exports are already prepared.
- It does not run REW/Audacity capture steps.
- Supported layouts:
  - one 6-channel WAV/FLAC per direction (`layout: multichannel`)
  - six mono WAV/FLAC files per direction (`layout: per_mic`)
  - mixed layouts by overriding `layout` per direction

Example:

```bash
python tools/calibration/compile_array.py \
  --manifest config/array_ir_manifest.example.yaml \
  --output-prefix build/array_profile_measured
```

Notes:

- Optional `calibration_yaml` import applies `gain_linear` and `polarity` only.
- `delay_samples` is ignored because synchronized IR phase already encodes delay.
- Sample-rate mismatch and channel-count mismatch are hard errors (no resampling).
- Producing a measured SMV3 artifact does not satisfy the M3 hardware-evidence gate by itself.

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

## REW `.mdat` ingest

`tools/calibration/mdat_parse.py` reads REW Measurement Data File V2 and extracts
embedded note strings:

- per-measurement Delay (ms) and distance note
- timing / measurement peak levels (dBFS)
- FR summary cards (frequency span, SPL span, date)
- source WAV names and array channel indices

It does **not** deserialize Java IR/FR sample arrays (that still needs REW's
classes or the REW HTTP API). Exported WAVs remain authoritative for GCC-PHAT.

## REW filter import policy

`tools/calibration/rew_filters.py` parses `filter-wholearray-formatted.txt` and emits:

- verbatim section list
- guarded section list (caps on Q, boost, and minimum frequency)
- dropped/tamed reasons
- worst-case cumulative boost estimate

The guarded list is intended for runtime use.

## Limits and assumptions

- Geometry in `config/geometry_soundbubble_initial.yaml` is provisional planar data (hardware-unverified).
- Measurement FFT (offline correlation diagnostic) is separate from runtime MVDR FFT (128/32). Spatial production calibration is the IR compiler, not this diagnostic.
- Do not mark M3 `done` until hardware gate evidence is recorded in `docs/milestones.md`.
- Exported IR peaks from REW are commonly normalized/positioned and are not timing-authoritative for delay estimation.
- Milestone gate evidence still requires hardware capture validation on target Pi.
