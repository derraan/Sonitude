# Sonitude Audio Algorithm Test Bench

A PySide6 GUI for testing the existing Sonitude beamforming / suppression
chain against pre-recorded six-microphone audio or a live six-channel
device, **without reimplementing production DSP in Python**. This is a
development/test tool, not a production deployment path and not a
low-latency substitute for `sonitude_realtime`.

## How the algorithm is reused

```
PySide6 GUI  →  Controller (QThread)  →  subprocess  →  Sonitude C++ CLI tool  →  DSP
                                                          (unmodified sonitude_core:
                                                           CalibrationApplier,
                                                           DelaySumBeamformer,
                                                           ConservativeSuppressor,
                                                           BinauralRenderer,
                                                           PeakLimiter / StereoPeakLimiter)
```

Two C++ CLI tools are used, both built from the repo's own `CMakeLists.txt`
and part of the portable (ALSA-free) subset, so they build on any platform:

| Tool | Existing / new | Used for |
| --- | --- | --- |
| `sonitude_wav_replay` | Existing (M4), extended with diagnostic taps, suppression AUTO/ON/OFF, `--capabilities`, and `--output-binaural` | Mode 1 — batch WAV processing |
| `sonitude_stream_process` | Protocol v2 stdin/stdout adapter (`src/tools/stream_process.cpp`) | Mode 2 — real-time streaming |

Both wrap the *same* `CalibrationApplier → DelaySumBeamformer → ConservativeSuppressor` chain as `sonitude_realtime`. Binaural rendering is applied in these tools (not yet in `sonitude_realtime`). `--output` from wav replay stays mono; stereo is `--output-binaural` or the stream payload.

## Response to code review (PR #32)

A review of the initial version found several real gaps: suppression could
never actually be enabled from the GUI, live capture ignored
`active_channel_map`, live recordings grew unboundedly in RAM, and steering
was commandable but not objectively testable, among others. Every item below
was fixed; the "Fix" column says where.

| Finding | Severity | Fix |
| --- | --- | --- |
| Suppression never enabled by the GUI (`enable_suppression=True` was never passed; no toggle existed) | Blocker | Added an "Enable Suppression" checkbox to both tabs, defaulted from `config`'s `suppression.enabled`, passed through to both controllers. `sonitude_wav_replay` was also fixed to fall back to `runtime.suppression.enabled` when `--enable-suppression` isn't passed, matching `sonitude_stream_process`'s existing behavior — see "What changed in the C++ tree". |
| Steering width completely absent from the widget, protocol, CSV, C++ API, and metadata | Blocker | Defined "width" as a directivity blend (see "Steering width definition" below), implemented identically in both C++ tools, and propagated it through the full stack: `SteeringEvent.width_deg` → steering script's 4th column → stream protocol's `width_deg` field → `SteeringDial`'s shaded wedge → `metadata.json`/`metrics.json`. |
| Steering is commanded but never objectively tested | High | Added `app/analysis/steering_sweep.py`: an energy-based beam-response sweep that re-renders the SAME recording through the unmodified algorithm at a range of candidate azimuths and reports the measured energy-peak direction against a user-entered expected direction. Wired into the Recorded Data tab as an opt-in "Objective Steering Test". |
| Live capture ignored `active_channel_map`; a non-identity map like `[2,4,6,8,10,12]` silently mismapped channels | Blocker | `RealtimeWorker` now opens `max(active_channel_map) + 1` device channels and applies the map (select + reorder) before anything reaches calibration/DSP — see `_select_active_channels` in `realtime_controller.py`, unit-tested in `tests/test_realtime_controller.py`. |
| Live recordings accumulated unboundedly in RAM (~1.4MB/s) | Medium | `RealtimeWorker` now streams recording blocks straight to temp WAV files via `soundfile.SoundFile` as they arrive; only file handles are held in memory regardless of session length. |
| RAW/PROCESSED domains conflated in the before/after UI | Medium | Added an explicit on-screen note distinguishing the ear-cup listening preview from the algorithm's real signal path, and split RESIDUAL into a selectable beamform-stage vs. limiter-stage choice rather than one hardcoded file. |
| "SII" implied a standards-compliant measurement | Medium | Renamed the displayed metric and its `metrics.json` key from `sii` to `intelligibility_proxy`, and the UI group title to "Experimental Intelligibility Proxy (not a standardized SII)". The underlying module (`app/analysis/sii.py`) already disclosed this in its docstring; now the UI does too. |
| No automated test exercised the real C++ pipeline | Medium | Added `tests/test_integration_wav_replay.py` and `tests/test_integration_stream_process.py`: real end-to-end tests (not mocks) that build/skip automatically based on whether the binaries exist, so they run for real wherever a C++ toolchain is available (e.g. CI) and skip cleanly here. They specifically assert that enabling suppression changes the output and that `width_deg` changes the output — the two things the review couldn't verify. |
| Unrelated PDF appears in the PR diff | Low | Verified locally: `git diff origin/main..feature/pyside6-testbench` does not include the PDF — it isn't part of this branch's commits. This is a stale PR-base artifact on GitHub's side (the PDF was fast-forward-merged to `main` separately, before this branch's PR was opened); rebasing/updating the PR's base against current `main` should clear it. |

## What changed in the C++ tree

- `src/tools/wav_replay.cpp`:
  - Added optional `--output-beamformed` and `--output-suppressed` flags
    that dump the mono signal immediately after the beamformer and
    immediately after suppression (both before the final `--output`, which
    is post-limiter). These are diagnostic taps only; the final render is
    unchanged. Needed because residual/noise metrics require same-domain,
    same-alignment signal pairs (see "Residual definition" below) that the
    tool didn't expose before.
  - Fixed suppression to also honor `runtime.suppression.enabled` from the
    config file (previously only `--enable-suppression` on the command line
    could turn it on, unlike `sonitude_stream_process` and
    `sonitude_realtime`, which both already read the config). `--enable-suppression`
    still forces it on regardless of config.
  - Added an optional 4th steering-script column, `width_deg` (see "Steering
    width definition" below); 3-column scripts still parse unchanged
    (`width_deg` defaults to 0).
  - `--output-binaural` writes stereo via the requested `--binaural-backend`.
    `--output` remains 1-channel processed mono.
  - `--capabilities` prints protocol-v2 JSON so the GUI can list backends.
- `src/tools/stream_process.cpp` — framed stdin/stdout adapter: 6-channel PCM
  in, stereo PCM out, protocol v2 (`SBB2`/`SBO2`). Optional binaural rendering
  is controlled per block (flags + backend byte). No ALSA — Python owns the
  devices via `sounddevice` and pipes blocks through this process. Wire format
  is `testbench/app/processing/protocol.py`.
- `CMakeLists.txt` — registers the new `sonitude_stream_process` executable,
  linked against `sonitude_core` only (same pattern as `sonitude_wav_replay`).
PySide6 GUI  →  Controller (QThread)  →  subprocess  →  C++ CLI  →  sonitude_core DSP
```

| Tool | Role |
| --- | --- |
| `sonitude_wav_replay` | Mode 1 batch: decoded 6-channel WAV + steering script → mono + diagnostic taps |
| `sonitude_stream_process` | Mode 2 live: protocol v2 stdin/stdout blocks around the same DSP chain |

The C++ chain is still `CalibrationApplier → DelaySumBeamformer →
ConservativeSuppressor → PeakLimiter`. Python never implements HRTF/ITD.
Binaural controls are **capability-gated**: this tree reports only
`mono_reference` (L=R duplicate of directional mono). `itd_ild`,
`compact_hrtf`, and `full_hrtf_reference` are listed as unavailable until
the separate binaural DSP work lands.

## Building the C++ tools

From the repo root:

```sh
cmake -S . -B build
cmake --build build --target sonitude_wav_replay sonitude_stream_process
```

The test bench looks for binaries in this order:

1. An explicit path in code
2. `SONITUDE_BUILD_DIR`
3. Common CMake output dirs (`build/`, `build/Debug`, `build/Release`, …)

Missing tools raise `MissingBinaryError` with the search list.

`--capabilities` on either tool prints JSON for protocol version, suppression
modes, taps, and which binaural backends this build actually implements.

## Running the GUI

```sh
cd testbench
python -m venv .venv
.venv\Scripts\activate      # or: source .venv/bin/activate
pip install -r requirements.txt
python -m app.main
```

## Running the tests

```sh
cd testbench
pip install -r requirements-dev.txt
python -m pytest
```

Unit tests do not need audio hardware. Integration tests call the **real**
`sonitude_wav_replay` / `sonitude_stream_process` executables.

- If the tools are missing and `SONITUDE_REQUIRE_CPP` is unset, those tests
  **skip**.
- In CI, `SONITUDE_BUILD_DIR` is set and `SONITUDE_REQUIRE_CPP=1`, so a
  missing binary **fails** rather than counting as a pass.

Linux CI (`.github/workflows/linux-build.yml`) runs CMake, CTest, pip
install of `testbench/requirements-dev.txt`, then pytest.

```sh
# local equivalent of CI python step
export SONITUDE_BUILD_DIR=$PWD/build   # or your CMake dir
export SONITUDE_REQUIRE_CPP=1
cd testbench && python -m pytest
```

## Mode 1 — Recorded Data

Select WAV / FLAC / MP3 files (MP3 only if the installed libsndfile can
decode it). Decode preserves channel count and order; there is **no silent
downmix**. A stereo file may decode and still fail DSP validation when the
active configuration requires six microphones.

Python decodes to canonical float32, writes a temp WAV for the C++
WAV-only tool, then reads back taps. Changing the **final export**
container (WAV or FLAC) does not change DSP. Intermediate taps stay WAV.
MP3 export is not supported.

Per-batch controls: commanded azimuth, **Directional / Omni Blend**
(compatibility field `width_deg`; not HPBW), suppression AUTO / ON / OFF,
optional capability-gated binaural request, optional steering sweep.

Result directory (`testbench/data/results/TEST_<id>/`):

```
├── input_original.<ext>      # copy of the user's file
├── input.wav / input_decoded.wav
├── processed.wav             # final C++ mono (post-limiter)
├── processed_export.wav|.flac
├── processed_stereo.wav
├── beamformed.wav            # DSP tap, pre-suppression
├── suppressed.wav            # DSP tap, pre-limiter
├── binaural_stereo.wav       # present only if requested and the C++ tap ran
├── raw_preview_stereo.wav    # ear-cup listening preview — NOT binaural
├── residual_beamform.wav     # beamformed − suppressed
├── residual_limiter.wav      # suppressed − processed
├── steering_script.csv
├── runtime_config.yaml
├── input_metadata.json
├── metadata.json             # provenance (null where unknown)
└── metrics.json
```

## Mode 2 — Real-Time

The device must supply at least `max(active_channel_map) + 1` channels.
`RealtimeWorker` applies the map **before** DSP. Capture uses a small
bounded **drop-oldest / newest-wins** queue (default capacity 3; not
claimed universally optimal). The GUI shows queue depth, dropped-block
count, and overrun.

Blocks go to `sonitude_stream_process` over **protocol v2** (see below).
If the C++ process dies, PortAudio fails, the pipe breaks, or the protocol
is invalid, the worker emits `errorOccurred` and shuts down; START /
RESTART remain usable. RECORD streams to named temp WAV files, not RAM.

This path is for **correctness**, not the production latency budget in
`docs/architecture.md`.

## Metric definitions

### Residual

Do not subtract 6-channel input from mono output. Residuals are
same-domain DSP taps only:

- `residual_beamform.wav` = `beamformed − suppressed`
- `residual_limiter.wav` = `suppressed − processed`

`raw_preview_stereo.wav` is an uncalibrated ear-cup pair (map indices 4/5),
matching `main.cpp --mode passthrough`. It is **listening-only**, never
called binaural output, and never used in residual or objective metrics.

`sonitude_stream_process` uses a versioned framed protocol. Python and C++ must
stay in lockstep (`testbench/app/processing/protocol.py` and
`src/tools/stream_process.cpp`). Responses echo the request sequence.

- Input magic `SBB2` (`0x32424253`), 48-byte header, 6-ch float32 payload.
- Output magic `SBO2` (`0x324F4253`), 24-byte header, 2-ch float32 payload.
- Input flags: bit0 suppression focus, bit1 binaural enabled, bit2 follow steering.
- Output flags: bit0 suppression applied, bit1 binaural applied, bit2 binaural
  unavailable, bit3 mono reference.
- Binaural backend byte: `0` none, `1` mono_reference, `2` itd_ild,
  `3` compact_hrtf, `4` full_hrtf_reference.

Both tools also support `--capabilities` (JSON, `protocol_version: 2`). The GUI
hides backends that the binary does not advertise.

### Binaural renderer (C++ DSP)

The test bench does not implement HRTF/ITD in Python. It enables the C++
renderer through `--output-binaural` / stream flags. Backends:

- `mono_reference` — L=R copy of processed directional mono
- `itd_ild` — Woodworth ITD + broadband ILD
- `compact_hrtf` / `full_hrtf_reference` — SADIE II D2 tables under
  `data/hrtf/generic_sadie2_d2/`

Implementation notes: `docs/binaural_renderer.md`.
### Noise suppression (`noise_suppression`)

Mixture-power vs estimated or reference noise-power, **not** speech SNR.
`metrics.json` uses `mixture_to_noise_*_db` plus `snr_*` aliases for
compatibility. `method` is `"reference"` or `"estimated"`.

### Experimental intelligibility proxy (`intelligibility_proxy`)

**Not ANSI/ASA S3.5 SII.** Experimental, not standardized, not
acceptance-gating. A noise-only clip compared with itself can still yield
an intermediate proxy score; that is why it must not gate pass/fail.

### Directional / Omni Blend (`directivity_blend_deg` / `width_deg`)

`DelaySumBeamformer` has no native beamwidth. The test-bench control mixes
beamformed mono toward the six-microphone average (`0` = directional,
`180` = omni mix). This is **not** measured HPBW or cone width.
`sonitude_realtime` does not expose this blend.

### Steering error

Circular distance: `((measured − expected + 180) mod 360) − 180`.
+179° vs −179° is **2°**, not 358°. The optional energy-peak sweep still
searches a limited azimuth range (currently ±90°); wraparound math is
fixed independently of that range.

### Suppression AUTO / ON / OFF

| Requested | Resolved |
| --- | --- |
| AUTO | YAML `suppression.enabled` |
| ON | forced on |
| OFF | forced off, **including when YAML is true** |

Requested and resolved state are stored separately in `metadata.json`.
`--enable-suppression` / `--disable-suppression` remain aliases for ON / OFF.

### Protocol v2

Must stay in lockstep: `testbench/app/processing/protocol.py` and
`src/tools/stream_process.cpp`.

Input header 48 bytes (`SBB2`): version, message type, sequence,
frame count, flags, payload length, az/el/blend, binaural az/el, backend.
Output header 24 bytes (`SBO2`): echoed sequence, flags, stereo PCM.
Detected errors include invalid magic, unsupported version, truncation,
invalid payload length / frame count, and unexpected / stale sequence.

### Provenance

`metadata.json` records input path/format/rate/channels, channel map,
config / geometry / calibration paths, steering, requested/resolved
suppression and binaural (where known), limiter flag, output format,
protocol/capabilities version, and git commit. Unknown values are `null`,
never invented.

## Architecture (module map)

```
testbench/app/
├── main.py
├── config_reader.py
├── version_info.py              # version from CMakeLists; git SHA or null
├── ui/
│   ├── recorded_tab.py
│   ├── realtime_tab.py
│   ├── steering_dial.py
│   ├── playback_panel.py        # listening preview / processed / residual
│   ├── visualization_panel.py
│   └── metrics_panel.py
├── controller/
│   ├── batch_controller.py
│   └── realtime_controller.py
├── audio_io/
│   ├── audio_loader.py          # WAV/FLAC/MP3 → float32; codec vs DSP errors
│   ├── wav_loader.py            # compatibility re-export
│   ├── exporter.py              # WAV/FLAC only
│   ├── block_queue.py           # drop-oldest queue
│   ├── device_manager.py
│   ├── playback_engine.py
│   └── downmix.py               # ear-cup preview only
├── processing/
│   ├── sonitude_binary_locator.py
│   ├── protocol.py              # stream protocol v2
│   ├── capabilities.py          # --capabilities JSON
│   ├── suppression.py           # AUTO/ON/OFF
│   ├── batch_adapter.py
│   └── stream_adapter.py
├── analysis/
│   ├── residual.py
│   ├── noise_suppression.py
│   ├── sii.py                   # experimental proxy only
│   ├── steering_error.py        # circular distance
│   └── steering_sweep.py
└── storage/
    ├── models.py
    └── result_store.py
```

## What changed in the C++ CLI tools (test-bench only)

These tools wrap `sonitude_core`. They do **not** change
`DelaySumBeamformer` / `ConservativeSuppressor` / `PeakLimiter` classes.

`sonitude_wav_replay`:

- Diagnostic taps: `--output-beamformed`, `--output-suppressed`, optional
  `--output-binaural` (`mono_reference` only in this tree)
- `--suppression auto|on|off` (plus `--enable-suppression` /
  `--disable-suppression`)
- `--capabilities`
- `active_channel_map` is bounds-checked against the input WAV
- 4th steering-script column is directivity blend (`width_deg` alias)
- stderr line `sonitude_resolved {…}` JSON

`sonitude_stream_process`:

- Protocol v2 (replaces the old 24-byte header)
- Same suppression / capabilities / blend behaviour
- No ALSA; Python owns devices via `sounddevice`

## Known limitations

- Not a low-latency production path (subprocess round-trip per block).
- HRTF/ITD DSP is not in this tree; GUI must not pretend unimplemented
  backends work.
- Intelligibility proxy is experimental and must not gate acceptance.
- Directional / Omni Blend is a test-bench mix, not physical beamwidth.
- Optional steering sweep still searches ±90° by default; circular error
  is used when an estimate exists.
- Mode 1 steering / blend / suppression apply per batch, not per file.
- Hardware array capture has not been used as an automated gate.
- Crash-before-save realtime recordings can leave temp WAVs in the OS temp
  directory.

## Related documents

- `docs/pr32_testbench_software_proposal.md` — app-side requirements for this phase
- `docs/binaural_renderer_dsp_proposal.md` — separate C++ HRTF/ITD work
- `docs/architecture.md` — production RT pipeline (this GUI is outside that budget)
