# Calibration

Milestone 3 provides evidence-driven per-channel calibration for the six-microphone Sonitude host DSP. Calibration data is estimated offline, validated against geometry and runtime sample rate, and consumed at runtime by `CalibrationApplier` (time domain) and `MvdrBeamformer` (steering delay phase).

Status: **IMPLEMENTED (offline/synthetic)** · **HARDWARE-EVIDENCE-PENDING**

Do not mark M3 `done` until hardware gate evidence is recorded in `docs/milestones.md`.

## Three tool families (do not mix their jobs)

| Family | Entry | What it is | Authoritative audio |
| --- | --- | --- | --- |
| **Python M3 compiler** | `python -m tools.calibration` or test-bench **Calibration** tab | Production scalar calibration (delay / gain / A–E YAML) from exported WAVs | Exported WAVs + optional stimulus WAV |
| **C++ capture / estimate** | `sonitude_calibration_capture` + `sonitude_calibration_estimate` | Channel diagnostic (DC, gain, normalized-correlation lag, polarity) | Live 6-ch capture WAV |
| **SMV3 array compiler** | `tools/calibration/compile_array.py` | Measured spatial profile for `spatial.backend: fixed_measured` | Synchronized 6-ch sweep recordings (+ stimulus) or pre-exported IR WAV/FLAC |

REW `.mdat` is **optional metadata** for the Calibration tab (delay / peak / FR notes). It is **not** an IR deserializer and is **not** an input to `compile_session`. Exported WAVs remain authoritative for GCC-PHAT / absolute TOF.

Test-bench sequence:

1. **Calibration** — import WAVs, compile A–E YAML.
2. **Apply variant to Recorded / Real-Time** — both DSP tabs load that runtime+calibration pair (dropdowns show the files). A running Real-Time stream is restarted.
3. Listen and tune beamforming / suppression on those tabs.
4. **Upload** — commit calibration + common EQ + those DSP knobs into `config/default.yaml` (the Sonitude pipeline config).

---

## Channel map

USB / WAV channel index `0..5` matches `config/geometry_soundbubble_initial.yaml`. REW notes use **1-based** `Channel 1..6` (same physical order: REW Channel *N* = USB index *N−1*).

| Index | ID | Role |
| ---: | --- | --- |
| 0 | `M0_left_ear` | Left ear |
| 1 | `M1_left_arc` | Left arc |
| 2 | `M2_left_top` | **Reference** (`steering.reference_mic_index`, compiler `REFERENCE_INDEX = 2`) |
| 3 | `M3_right_top` | Right top |
| 4 | `M4_right_arc` | Right arc |
| 5 | `M5_right_ear` | Right ear (C/D/E invert-test target) |

Array WAVs must have **at least six** channels; the compiler uses the first six. Element WAVs must be **mono**. All array, element, and stimulus files for one compile must share the **same sample rate** (no resampling). Extra channels beyond six on an array WAV are ignored.

---

## File naming convention (required for REW `.mdat` safety)

The `.mdat` parser classifies measurements from **source WAV names and nearby notes**, not from channel count inside the binary. Use these stems so array takes are never treated as element takes and vice versa.

### Array (simultaneous 6-channel) takes

Include **`MIC-array`** or **`array-`** and an azimuth tag:

```text
MIC-array-0deg.wav
MIC-array-+30deg.wav
MIC-array--90deg.wav
```

Azimuth parser (`tools/calibration/angles.py`): a token like `0deg`, `+30deg`, `-90deg` in the path or notes. That number is **source-vs-nose**, not head-turn angle (see [Azimuth convention](#azimuth-convention-fixed-loudspeaker--head-yaw)).

Default characterization grid (1 m, loudspeaker fixed, GUI rows are bypassable):

`0°, ±30°, ±60°, ±90°, ±120°, ±140°, ±160°, ±180°`

Only **0°** is enabled by default. Primary azimuth (prefers 0°) feeds runtime YAML; other enabled angles are estimated into the report only.

### Element (same-position, one mic) takes

Use the exact stem **`MIC-M0` … `MIC-M5`**. Do **not** put `array-` or `mic-array` in these names:

```text
MIC-M0.wav
MIC-M1.wav
…
MIC-M5.wav
```

`M0.wav` or `mic0.wav` is **unsafe** for `.mdat` ingest: if REW also wrote `Channel N`, the parser treats that row as an **array** channel.

### Stimulus (played sweep / MLS / ESS)

Any name is fine for compile (`--stimulus-wav`). Keep it out of the `MIC-array` / `MIC-Mx` patterns if you also drop it into the same `.mdat`.

### How `.mdat` classification works

```text
is_array_channel  iff  "mic-array" in text
                   or  "array-" in text
                   or  (Channel N present AND "mic-m" not in text)

is_element        iff  name/title matches MIC-M[0-5]
```

`array-` wins even if `MIC-M0` is present. If no measurement matches the requested azimuth, `array_channel_delays_ms()` falls back to **every** array-classified row — a misnamed element can pollute the 0° delay preview.

Compile **does not** select WAVs from the `.mdat`. You still assign array WAVs in the azimuth table and element WAVs in the M0–M5 slots. Bad names only corrupt MDAT preview unless you also pick the wrong file in those slots.

---

## What a REW `.mdat` must contain

`tools/calibration/mdat_parse.py` reads **REW Measurement Data File V2** (Java serialization):

- File starts with magic `\xac\xed\x00\x05`
- First 80 bytes contain `REW Measurement Data File`

It scans nearby plaintext; it does **not** deserialize Java IR/FR sample arrays (that still needs REW classes or the REW HTTP API).

Per measurement it looks for:

- `Delay <value> ms` (optional distance in parentheses)
- `Channel N` (1-based) on array takes
- source `.wav` path/name (so the naming rules above apply)
- `Timing signal peak level … dBFS` / `measurement signal peak level … dBFS`
- optional HTML FR cards (Hz span, SPL span, date)
- optional `Clock adjustment: … ppm`

Sample rate is a **guess** from names/notes (`44k`/`44100`, `48k`/`48000`, `96k`/`96000`).

If delay notes are missing, parse succeeds with zero measurements and a warning. That is not a compile failure.

REW’s ~32-measurement cap is handled in the Calibration tab: **Import REW .mdat…** accepts multiple files (`merge_mdat_results`). Later files overwrite the same azimuth+channel. Exported WAVs remain authoritative for GCC-PHAT.

**Recommended REW export for calibration:** keep the `.mdat` file(s) for notes, and **export WAVs** (6-ch array per azimuth, mono elements, played stimulus) for the compiler.

---

## Azimuth convention (fixed loudspeaker / head yaw)

DSP azimuth is **head-fixed source direction**: where the loudspeaker sits relative to the **nose**, not how far the headset was turned, and not a room compass.

| File / GUI label | Where the speaker is, relative to the nose |
| --- | --- |
| `0deg` | On the midsagittal plane (straight ahead) |
| `+30deg`, `+90deg`, … | Toward the **wearer’s right** |
| `-30deg`, `-90deg`, … | Toward the **wearer’s left** |
| `±180deg` | Directly behind |

Positive azimuth is **clockwise when looking down on the head** (from the crown toward the feet). It is **not** “clockwise from a camera facing the listener.”

WAV names and compiler `--azimuth-deg` / GUI rows always use this **source-vs-nose** angle. Never name a file after the head-turn angle with the same sign.

### Locked measurement protocol (loudspeaker stays put)

This is the characterization protocol when the speaker cannot move.

1. Put the loudspeaker on a room mark. Call that mark **room 0°**. Sit so the **head centre** (midpoint between ear mics) is **1 m** from the speaker. Keep that centre on the same point for every take (rotate in place; do not walk an arc).
2. Tape a **nose line** on the floor or use a headband protractor / laser on the sagittal plane.
3. **0° take:** face the speaker. Nose line points at room 0°. Save `MIC-array-0deg.wav`.
4. For a labeled angle **θ** (the value in the filename / GUI): yaw the headset by **−θ**.
   - **Law:** `source_azimuth = −head_yaw`, with **head_yaw > 0** = nose turns toward the **wearer’s right**.
5. Recheck 1 m and elevation 0° (speaker at ear / array height). Record. Return the nose to room 0° between takes if the chair drifts.

Worked examples (speaker fixed at room 0°):

| You want file / row | Turn the nose | Check |
| --- | --- | --- |
| `+30deg` | **30° to the wearer’s left** | Speaker is 30° right of the nose; right ear is farther from the speaker than the left |
| `-30deg` | **30° to the wearer’s right** | Speaker is 30° left of the nose; left ear is farther than the right |
| `+90deg` | Nose 90° left (left ear toward the speaker) | Speaker off the right ear |
| `-90deg` | Nose 90° right (right ear toward the speaker) | Speaker off the left ear |

If you yaw **30° right** and save `+30deg`, the geometry term and steering sign are **flipped**. Do not do that.

Element takes (`MIC-M0` … `MIC-M5`) stay at the **0°** pose (nose toward the speaker).

Same-position note: rotating the head with a fixed speaker is acoustically equivalent to moving the speaker on the 1 m circle **only if** the rotation axis is the array origin. Chair translation, leaning, or pivoting about the feet will mix azimuth with distance change.

---

# User guide 1 — Test-bench Calibration tab (recommended)

The PySide6 **Calibration** tab (`testbench/app/ui/calibration_tab.py`) is a GUI for `python -m tools.calibration`. It does not run production DSP in Python.

Default output folder: `testbench/data/calibration/cal_<YYYY-MM-DD>/`.

### 1. Prepare files

1. Capture / export one **6-channel** array WAV per azimuth you will enable. Name them `MIC-array-<az>deg.wav` using **source-vs-nose** azimuth (see [Azimuth convention](#azimuth-convention-fixed-loudspeaker--head-yaw)). With a fixed speaker, yaw the head the **opposite** way to the label.
2. Capture / export six **mono** element WAVs at the same position: `MIC-M0.wav` … `MIC-M5.wav`.
3. Export the **played stimulus** WAV (same sample rate as the array) if using Absolute TOF (default).
4. Confirm geometry YAML (tab defaults to the path in `config/default.yaml`, typically `config/geometry_soundbubble_initial.yaml`).
5. Optional: REW `filter-wholearray-formatted.txt` (whole-array EQ, not per-mic).
6. Optional: one or more REW `.mdat` files for delay/level preview (Ctrl+click to select a split pair).

### 2. Import array sweeps

1. Open the test bench → **Calibration**.
2. In **Array sweeps by azimuth (1 m)**, leave **0°** checked (or **Only 0°**).
3. Select the 0° row → **Import WAV for row** → choose `MIC-array-0deg.wav`.
4. For extra angles: check **Use**, import that row’s 6-ch WAV. **Enable all** only if every row has a file.
5. Unchecked rows are bypassed and never compiled.

### 3. Optional: import `.mdat` (preview only)

1. **Import REW .mdat…** and select one or more files (split exports are merged; last file wins on the same azimuth+channel).
2. The report pane shows a markdown table of parsed Delay / Channel / azimuth / peak / FR notes.
3. Matching azimuth rows fill **MDAT delay ms** (mean of array-classified channels) and **Peak dBFS**.
4. Hover the delay cell for per-channel `chN:…ms` (REW 1-based).
5. If element takes appear in array delays, rename those WAVs in REW to `MIC-Mx` and re-import. Do not treat this table as compile input.

### 4. Shared imports

1. Import each **Same-position element sweep** (`M0_left_ear` … `M5_right_ear`) — order is USB 0..5.
2. **Import stimulus** for Absolute TOF (required unless Delay mode is Relative).
3. Optionally **Import REW filters**.
4. Set **Output folder** and **Artifact tag** (files become `calibration_<tag>_*.yaml`).

### 5. Compiler controls

| Control | Default | Meaning |
| --- | --- | --- |
| Primary azimuth | 0° among enabled rows | Feeds runtime YAML `delay_samples` |
| Delay mode | Absolute TOF | Stimulus → each mic; relative is mic-vs-mic GCC-PHAT (no stimulus) |
| Source elevation | 0° | Geometry term |
| Source distance | 1.0 m | Spherical TOF + auto max-TOF window |
| Wavefront | Spherical (1 m) | Or plane wave |
| Max relative lag | 4410 samples | Relative mode search ±N |
| Max TOF | auto | Absolute window; 0 = `distance/c + 100 ms` (min 256) |
| Polarity threshold | 0.2 | Report-only ambiguity; emitted polarity is still +1 except M5 invert-test |
| Gain source | Element sweeps | Or array sweep; 300–8000 Hz RMS vs reference |
| REW max Q / boost / min Hz | 4 / 6 dB / 100 Hz | Guards imported common EQ |
| M5 invert-test | on | C/D/E set `M5_right_ear` polarity to −1 |

### 6. Compile

1. **Compile calibration**.
2. Wait until the report pane shows JSON (and MDAT markdown if imported).
3. Expect in the output folder:
   - `calibration_<tag>_A_baseline.yaml`
   - `calibration_<tag>_B_delay.yaml`
   - `calibration_<tag>_C_polarity.yaml`
   - `calibration_<tag>_D_delay_polarity.yaml`
   - `calibration_<tag>_E_full.yaml`
   - `calibration_report.json`
   - `calibration_report.md`

### 7. Apply to Recorded / Real-Time (session overlay)

1. Choose a **Variant** (see [A–E variants](#ae-variants) below). Typical listen path: **E_full**; control: **A_baseline**.
2. Optionally check **Enable guarded REW common EQ in DSP overlay**.
3. **Apply variant to Recorded / Real-Time**. This writes `runtime_config_overlay.yaml`, points both DSP tabs at that runtime YAML plus the variant calibration YAML, and restarts a running Real-Time stream. Recorded / Real-Time show **Runtime YAML** and **Calibration YAML** dropdowns so you can verify or pick another file. Next **Process** / **START** uses that pair.
4. This does **not** rewrite `config/default.yaml`. That is the Upload step.

### 8. Commit to the Sonitude pipeline (Upload tab)

1. Tune beamforming / suppression / binaural on **Recorded** or **Real-Time**.
2. Open **Upload**. **Commit DSP settings from** the tab you just tuned.
3. Confirm the calibration variant snapshot (from the Calibration tab).
4. **Commit to RT YAML**.
5. Upload backs up `config/default.yaml`, copies the variant to `config/calibration_uploaded.yaml`, and patches `default.yaml`: calibration path, suppression knobs, steering ambient floor, binaural, and guarded `common_eq` if enabled on Calibration.
6. Recorded and Real-Time dropdowns switch to `default.yaml` + `calibration_uploaded.yaml`. A running Real-Time stream is restarted.

---

# User guide 2 — Python compiler CLI

From the repo root:

```text
python -m tools.calibration ^
  --array-wav path\to\MIC-array-0deg.wav ^
  --element-wavs path\to\MIC-M0.wav path\to\MIC-M1.wav path\to\MIC-M2.wav path\to\MIC-M3.wav path\to\MIC-M4.wav path\to\MIC-M5.wav ^
  --geometry config\geometry_soundbubble_initial.yaml ^
  --out-dir testbench\data\calibration\cal_session ^
  --tag session ^
  --azimuth-deg 0 ^
  --elevation-deg 0 ^
  --distance-m 1.0 ^
  --wavefront spherical ^
  --delay-mode absolute_tof ^
  --stimulus-wav path\to\stimulus.wav
```

On POSIX shells, use `\` line continuations instead of `^`.

### Required

- `--array-wav` — 6-channel (or wider) WAV; first six channels = M0..M5
- `--element-wavs` — exactly six **mono** paths in M0..M5 order
- `--geometry` — six-mic YAML with IDs in the table above
- `--out-dir`

`--azimuth-deg` (default 0) is **source-vs-nose** for `--array-wav`, not head yaw. Fixed-speaker capture: yaw by the negative of this value. See [Azimuth convention](#azimuth-convention-fixed-loudspeaker--head-yaw).

### Delay

- `--delay-mode absolute_tof` (default) requires `--stimulus-wav` at the same sample rate. First stimulus channel is used if the file is not mono.
- `--delay-mode relative` — mic-vs-mic GCC-PHAT; stimulus not required.
- `--max-tof-samples 0` — auto (`distance/c + 100 ms`).
- `--max-lag-samples` — relative-mode window (CLI default **100**; GUI default **4410**).

### Extra azimuths (report only)

```text
--extra-array-angle 30 path\to\MIC-array-+30deg.wav
```

Repeatable. `30` here is source-vs-nose (**+30°** = speaker on the wearer’s right). Runtime YAML still uses `--azimuth-deg` / `--array-wav` only. The report gets a `multi_angle` warning.

### Optional

```text
--gain-source element|array
--rew-filter-txt path\to\filter-wholearray-formatted.txt
--rew-max-q 4 --rew-max-boost-db 6 --rew-min-freq-hz 100
--no-m5-invert-test
--polarity-threshold 0.2
--tag session
```

`.mdat` is GUI-only; there is no `--mdat` flag.

### Missing-input checks (GUI)

`missing_compile_inputs()` lists absent files before compile: 6-channel array WAV, each element WAV, geometry, REW filter text if a path was set, and stimulus WAV when delay mode is `absolute_tof`.

---

# User guide 3 — Hardware characterization checklist

Loudspeaker stays put. Yaw the headset using [Azimuth convention](#azimuth-convention-fixed-loudspeaker--head-yaw): `source_azimuth = −head_yaw` (positive yaw = nose right). File tags are still source-vs-nose (`+30deg` means speaker on the wearer’s right).

Record for each run: date, hardware revision, geometry file, sample rate, capture files, artifact paths, command, result, operator notes.

| Step | Purpose | Expected files |
| --- | --- | --- |
| Channel activity / order | Confirm USB map 0..5 | Scratch 6-ch WAV |
| Silence | DC / ambient (C++ estimator) | Capture with silence region |
| Common-source / stimulus | Gain, TOF, polarity | Played stimulus + 6-ch array |
| Same-position elements | Per-mic gain / matching EQ fit | `MIC-M0` … `MIC-M5` mono |
| Azimuth grid at 1 m (fixed speaker: yaw head by **−θ**) | Multi-angle report; primary 0° for YAML | `MIC-array-<az>deg.wav` |
| Repeat capture | Repeatability | Second set of WAVs |
| Front / left / right anchors | Geometry + delay-sign closure | 0° face speaker; +90° yaw nose **left**; −90° yaw nose **right** |
| MVDR replay A vs E | End-to-end steering | Overlay apply + Recorded tab |

This checklist is **not yet the M3 hardware-evidence gate**. Producing YAML or an SMV3 profile does not set `quality.hardware_evidence`.

---

# User guide 4 — C++ capture / estimate (channel diagnostic)

This path is **not** the Python IR/TOF compiler. It writes schema-v2 YAML with DC, relative gain, normalized **time-domain** correlation lag (not GCC-PHAT spectral weighting), and polarity (or UNRESOLVED).

`sonitude_calibration_capture` writes a six-channel WAV with:

1. **Silence region** (~0.5 s): DC offset, ambient-noise check.
2. **Common-source region** (~2 s): multi-tone for gain, polarity, delay.

Channel order matches geometry USB indices 0..5.

```bash
./build/sonitude_calibration_capture --output build/calibration_capture.wav
./build/sonitude_calibration_estimate build/calibration_capture.wav build/calibration_estimate.yaml \
  --geometry config/geometry_soundbubble_initial.yaml \
  --reference-index 2 \
  --report build/calibration_report.txt
```

Windows: `build/sonitude_calibration_capture.exe` and `sonitude_calibration_estimate.exe`.

Deterministic software validation:

```bash
./build/sonitude_calibration_capture --output build/calibration_capture_synth.wav --synthetic-test
./build/sonitude_calibration_estimate build/calibration_capture_synth.wav build/calibration_estimate_synth.yaml \
  --geometry config/geometry_soundbubble_initial.yaml --report build/calibration_report_synth.txt
```

Pass `--hardware-evidence` on the estimator only when a real capture on target hardware is being asserted. `quality.valid` is true only when estimation status is PASS **and** `hardware_evidence` was asserted. Parsing a valid YAML file does not imply a valid calibration.

Companion report fields: per-channel DC, gain, relative gain dB, delay (samples and µs), polarity (or UNRESOLVED), correlation peak, delay confidence, RMS, clipping flag, warnings.

Statuses: `PASS`, `WARNING`, `UNRESOLVED`, `INVALID`.

---

# User guide 5 — Physical measured-profile compiler (SMV3)

`tools/calibration/compile_array.py` emits an SMV3 profile (`.bin`, `.npz`, `.csv`, `.report.json`) for `spatial.backend: fixed_measured`.

Input modes:

- `input: sweep` (schema v2): six-channel sweep recordings + known stimulus WAV. The compiler deconvolves each channel, applies one shared direct-path window per azimuth, then derives complex RTF steering.
- `input: ir` (schema v1/v2): already exported synchronized IR WAV/FLAC.

Layouts:

- `multichannel`: one 6-channel WAV/FLAC per direction (**exactly** 6 channels)
- `per_mic`: six mono files per direction in USB order M0..M5
- mixed: override `layout` per direction

```bash
python tools/calibration/compile_array.py \
  --manifest config/array_ir_manifest.example.yaml \
  --output-prefix build/array_profile_measured
```

Notes:

- Optional `calibration_yaml` applies `gain_linear` and `polarity` only.
- `delay_samples` is ignored: synchronized IR phase already encodes delay.
- Sample-rate mismatch and channel-count mismatch are hard errors.
- For sweep mode, window policy and detected direct-arrival offsets are written into `.report.json`.
- This artifact does not satisfy the M3 hardware-evidence gate by itself.

Example manifest shape is in `config/array_ir_manifest.example.yaml`.

---

## Calibration model (runtime)

Per microphone `m`, the time-domain path applies:

```text
x_cal,m[n] = polarity[m] × gain[m] × (x_m[n] - dc_offset[m])
```

followed by the DC blocker (`calibration_dc_block_hz`, default 20 Hz) and optional per-mic EQ (`eq.enabled` + biquad sections).

The MVDR steering vector applies fractional calibration delay as **phase** (legacy geometric backend):

```text
d_m(k, θ) = exp(j 2π k Δ_m(θ) / N)
Δ_m(θ) = geometry_delay_m(θ) + calibration_delay_m - reference_delay
```

`delay_samples` is **not** applied in `CalibrationApplier` (avoids double correction).

Production measured steering (`fixed_measured`) does **not** use YAML `delay_samples`. Complex RTFs from synchronized IRs already contain delay in their phase.

### Reference microphone convention

- Default reference: index **2** (`M2_left_top`).
- Gain: each channel’s 300–8000 Hz RMS is scaled to the reference RMS.
- Delay sign: positive `delay_samples` advances that channel relative to the reference in **legacy geometric** steering phase.
- Python compiler: measured lag is GCC-PHAT (relative) or stimulus-matched TOF (absolute); runtime delay is `-(lag) - tau_geom`, then re-zeroed at the reference.
- C++ diagnostic: stores the negative of the normalized correlation lag so late channels receive compensating phase.

### Python compiler delay modes

- **`absolute_tof` (default):** bandpass (300–3k, 3k–8k, 300–8k), correlate each mic against the **played stimulus** (`mic * conj(stimulus)`), take the 300–8000 Hz band. Relative lag is `tof_i - tof_ref`. Then remove geometry for YAML `delay_samples`.
- **`relative`:** mic-vs-mic GCC-PHAT in the same bands, ±`max_lag_samples`.

Parabolic interpolation refines the correlation peak. If low-band vs high-band lags differ by more than 1 sample, the report warns `band_dependent_lag`.

### Polarity and gain in emitted YAML

- Polarity **detection** is written to the report (signed correlation at the selected lag vs `--polarity-threshold`).
- Emitted YAML polarity is **always +1** on A/B. C/D/E invert **M5 only** when M5 invert-test is on (`ch6_invert_test`). Detected inversions on other mics are **not** written into A–E.
- Python compiler sets `dc_offset: 0.0`. Use the C++ estimator if you need measured DC.
- Per-mic matching EQ is fitted from element-band RMS vs reference (centers 650 / 2000 / 5200 Hz, gain clipped ±6 dB, skip if |gain| < 0.5 dB) but stored with **`eq.enabled: false`**. Enable sections in YAML only after review.

---

## A–E variants

| File suffix | Delay | Gain | Polarity |
| --- | --- | --- | --- |
| `A_baseline` | 0 | 1.0 | all +1 |
| `B_delay` | measured | 1.0 | all +1 |
| `C_polarity` | 0 | 1.0 | M5 −1 if invert-test |
| `D_delay_polarity` | measured | 1.0 | M5 −1 if invert-test |
| `E_full` | measured | measured | M5 −1 if invert-test |

Python `emit_variants` writes a compact mapping (`sample_rate_hz` + `channels`). That is schema **v1-style** (no `schema_version`). The C++ estimator writes **schema v2** (`identity`, `capture`, `quality`, …). The runtime loader accepts both (v1 = missing `schema_version`).

---

## Artifact schema (v2, C++ estimator)

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

Validation rules (loader):

- exactly six channels, one per geometry microphone ID
- `polarity` in `{+1, -1}`
- `gain_linear` in `(0, 8]`
- `|delay_samples| <= 256`
- per-channel EQ: at most 24 sections
- section `type` in `PK | LS | HS | LP | HP`
- section `freq_hz` in `(0, sample_rate/2)`
- section `q` in `(0, 20]`
- section `|gain_db| <= 24`

Loader also rejects: sample-rate mismatch vs runtime, duplicate/unknown IDs, non-finite values, missing reference microphone.

---

## Runtime common EQ

One **common** mono EQ after beamforming and before limiting:

```yaml
common_eq:
  enabled: false
  sections: []
```

This is voicing, not per-mic calibration. Guarded REW sections can be injected by **Apply variant** when the checkbox is on.

### REW filter import policy

`tools/calibration/rew_filters.py` parses `filter-wholearray-formatted.txt` rows like:

```text
<idx> True Auto PK <freq_hz> <gain_db> <Q> …
```

- Allowed types: `PK`, `LS`, `HS`, `LP`, `HP` (`NONE` skipped)
- **Verbatim** list kept in the report
- **Guarded** list: drop below min frequency; cap Q; clip gain to ±max boost
- Dropped/tamed reasons and a worst-case cumulative boost estimate are reported
- Guarded list is what runtime overlay may enable

---

## Limits and assumptions

- Characterization uses a **fixed loudspeaker**; yaw the head by `−θ` for a file labeled `θ` ([Azimuth convention](#azimuth-convention-fixed-loudspeaker--head-yaw)).
- Geometry in `config/geometry_soundbubble_initial.yaml` is provisional planar data (hardware-unverified).
- Speed of sound in the Python geometry term is **343 m/s**.
- Measurement FFT (offline correlation) is separate from runtime MVDR FFT (128/32). Spatial production calibration is the SMV3 IR compiler, not the scalar YAML diagnostic.
- REW-exported IR **peaks** are often normalized/positioned and are not timing-authoritative; use raw array WAVs + stimulus for delay.
- Frequency-dependent complex residual correction is **not established**. Scalar gain + fractional delay remain the runtime model until measured sweeps show material mismatch after scalar calibration.
- Milestone gate evidence still requires hardware capture validation on the target Pi.

## Future firmware serialization boundary

```text
Host YAML + report
    → firmware calibration packer (future repo)
    → versioned binary artifact (schema, sample rate, topology, gain, delay, optional Q15 residual, CRC)
    → future MCU loader
```

Dual-slot Flash/CRC semantics belong to the embedded integration boundary and are not implemented in this repository.
