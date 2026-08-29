# Sonitude Audio Algorithm Test Bench

A PySide6 GUI for testing and evaluating the existing Sonitude beamforming/
suppression algorithm — against pre-recorded 6-channel WAV files or a live
6-channel microphone — without reimplementing any of the DSP. This is a
**development/test tool**, not a production deployment path.

## Why this exists

Sonitude's algorithm (`src/dsp/`) is C++, built with CMake. This test bench
is a separate Python application that drives the *existing, unmodified*
algorithm through its own CLI tools and compares raw vs. processed audio,
computes metrics, and visualizes results. No DSP math lives in Python.

## How the algorithm is reused (not reimplemented)

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

## Building the C++ tools

From the repo root:

```sh
cmake -S . -B build -DSONITUDE_BUILD_TESTS=OFF
cmake --build build --target sonitude_wav_replay sonitude_stream_process
```

The test bench looks for the built binaries (in order) in:
1. An explicit path you pass in code,
2. the `SONITUDE_BUILD_DIR` environment variable,
3. common CMake build directory names under the repo root (`build/`,
   `build/Debug`, `build/Release`, `out/build/default-debug`, ...).

If none are found you'll get a clear `MissingBinaryError` telling you what
was searched, not a cryptic `FileNotFoundError`.

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

This covers every non-GUI component: WAV validation, residual math, noise
suppression metrics, the intelligibility proxy, steering script parsing
(including `width_deg`), the streaming wire-protocol byte layout, the
`active_channel_map` selection/reordering logic, the bounded-RAM recording
lifecycle, and result storage — 43 tests, all runnable with no C++ binary or
audio hardware. Two more test files
(`test_integration_wav_replay.py`, `test_integration_stream_process.py`)
run real end-to-end checks against the actual C++ binaries and
**automatically skip** if they aren't built, rather than failing or being
faked with mocks — build the C++ tools first (see above) to exercise them.
GUI widget construction was additionally smoke-tested manually with a real
synthetic 6-channel WAV and `QT_QPA_PLATFORM=offscreen` (not part of the
automated suite, since it needs a Qt platform plugin), including a full
batch run that correctly fails with a clear `MissingBinaryError` when the
C++ tools aren't built rather than crashing.

## Mode 1 — Recorded Data

Select a WAV file or a folder of WAV files. Each is validated (≥6 channels,
exact sample-rate match to `config/default.yaml`'s `capture.sample_rate_hz`
— the pipeline does no resampling), then processed by `sonitude_wav_replay`
on a background `QThread` (`BatchWorker`) so the GUI stays responsive.

The steering dial, width slider, and "Enable Suppression" checkbox apply to
the whole batch (one commanded direction/width/suppression setting per run —
per-file overrides aren't supported yet, see "Extending this test bench").
Checking "Run steering sweep for this batch" additionally runs the objective
steering test (see "Steering (`steering` in metrics.json)" below) against
each file, using the "Expected source direction" you enter as ground truth.

For every input file this produces a `testbench/data/results/TEST_<id>/`
directory:

```
TEST_20260829_142233_ab12/
├── input.wav                 # copy of the original — never overwritten/moved
├── processed.wav             # final algorithm output (mono, as the algorithm produces it)
├── processed_stereo.wav      # processed.wav duplicated to stereo, for playback
├── beamformed.wav            # diagnostic tap: post-beamform, pre-suppression
├── suppressed.wav            # diagnostic tap: post-suppression, pre-limiter
├── raw_preview_stereo.wav    # ear-cup mic pair (ch 4/5), for A/B listening — see below
├── residual_beamform.wav     # beamformed - suppressed, stereo-duplicated
├── residual_limiter.wav      # suppressed - processed, stereo-duplicated
├── steering_script.csv       # exact steering commands used, for reproducibility
├── metadata.json             # test_id, input file, config, algorithm version, timestamp
└── metrics.json              # everything metrics_panel.py displays
```

## Mode 2 — Real-Time

Pick an input device, hit START. The device must supply at least
`max(active_channel_map) + 1` channels (6 for the default identity map
`[0,1,2,3,4,5]`; more for a sparse/reordered map like `[2,4,6,8,10,12]`) —
the GUI checks this and reports exactly how many channels are required
before starting. `RealtimeWorker` (a `QThread`) captures that many device
channels via `sounddevice`, applies `active_channel_map` to select and
reorder them into the six active mics (`_select_active_channels`) *before*
anything reaches calibration or the DSP chain, sends each block to a live
`sonitude_stream_process` subprocess along with the current steering-dial
azimuth/width, plays the returned stereo block immediately, and updates
level meters and a live waveform. RECORD streams raw and processed blocks
straight to temp WAV files (not RAM — see below); STOP + "Save
Raw/Processed Recording" copies the finished temp files to your chosen path.

This is architected as a **separate** controller from batch mode
(`RealtimeWorker` vs. `BatchWorker`) because streaming has different
buffering/latency constraints than one-shot file processing — see
`docs/architecture.md`'s real-time latency budget for why the production
path cares about this so much. This test bench is explicitly *not* trying
to hit that budget; it exists to validate correctness, not latency.

## Metric definitions (read before trusting a number)

### Residual definition

The beamformer changes channel count (6 → 1) and applies a per-channel
steering delay. **A raw `input − output` subtraction across that boundary is
not meaningful** — different channel counts, different time alignment. So:

- `residual_beamform.wav` = `beamformed − suppressed` (both mono, same
  alignment): exactly what the suppression stage removed. Meaningful because
  `ConservativeSuppressor` is a scalar broadband gain, not spectral — this
  is a valid difference, not an approximation.
- `residual_limiter.wav` = `suppressed − processed`: what the limiter
  changed.
- `raw_preview_stereo.wav` exists for **listening comparison only**. It is
  the ear-cup mic pair (channels 4/5 of `active_channel_map`), matching the
  one stereo convention that already exists in the codebase
  (`main.cpp --mode passthrough`). It is never subtracted from anything —
  see `app/analysis/residual.py`, which raises `IncompatibleSignalsError`
  if you try to subtract signals with different channel counts.

### Noise suppression metrics (`noise_suppression` in metrics.json)

Computed from `beamformed.wav` (before) vs. `suppressed.wav` (after) — the
same-domain pair immediately spanning the suppression stage. Every result
carries a `method` field:

- `"reference"` — an explicit noise-only clip was supplied for both signals;
  accurate SNR.
- `"estimated"` — no reference available (the default for a single
  recording with no separate noise clip). Noise floor is estimated from the
  quietest 20ms frames of the signal itself. **This only works if the
  signal actually has quiet segments** (e.g. speech with pauses); a
  continuously-present tone/noise mix will not separate cleanly — a real
  bug that showed up in this project's own test fixtures during
  development, before the fixture was changed to include realistic gaps.

### Experimental Intelligibility Proxy (`intelligibility_proxy` in metrics.json)

**Not a certified ANSI/ASA S3.5-1997 SII implementation** — this key was
renamed from `sii` specifically so it can't be mistaken for one. It sums
per-band importance weights times a per-band audibility function derived
from a simplified SNR model. The band importance weights in
`app/analysis/sii.py` are illustrative (shaped like the standard's
importance function, not transcribed from it) and are documented as such in
that file. Useful for relative before/after comparison on the same signal;
**do not use for certified, regulatory, or clinical claims.**

### Directional / Omni Blend (compatibility name: width_deg)

`DelaySumBeamformer` has **no native beamwidth / HPBW parameter**. The
test-bench control labeled **Directional / Omni Blend** mixes the
beamformer's mono output toward a simple average of the six calibrated
microphone channels, in proportion to `directivity_blend_deg / 180`
(stored as `width_deg` for compatibility with existing scripts).

This is **not** a measured physical beamwidth, cone width, or HPBW.

- `0` — fully directional (pure delay-and-sum)
- `180` — fully omnidirectional mix (six-mic average)
- Values in between blend linearly

### Protocol v2

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

### Suppression AUTO / ON / OFF

CLI `--suppression auto|on|off` stores requested vs resolved state separately.
`OFF` overrides YAML `suppression.enabled: true`. `AUTO` follows YAML. `ON`
forces the suppressor on.

### Input / output formats

Inputs: WAV, FLAC, and MP3 **when the installed libsndfile can decode them**.
A stereo file may decode successfully and still be rejected as a six-microphone
DSP input. The test bench does not silently downmix. MP3 encode/export is not
supported. Final result export is WAV or FLAC; changing the export container
does not change DSP. Intermediate taps remain WAV.

### Real-time capture queue

Live capture uses a small bounded **drop-oldest / newest-wins** queue. Capacity
is configurable (default 3 is a starting point, not a claim of optimality).
The GUI shows depth, dropped-block count, and overrun.

### CI

Linux CI runs CMake configure/build, CTest, installs Python test-bench
dependencies, and runs pytest with `SONITUDE_BUILD_DIR` and
`SONITUDE_REQUIRE_CPP=1` so missing C++ tools **fail** instead of skip.

### Steering (`steering` in metrics.json)

The Sonitude algorithm today only *consumes* a commanded steering target
(a script in batch mode, the dial in real-time mode) — nothing in the
pipeline itself *estimates* a direction from the signal. The mock DOA
provider replays scripted values as if they were ground truth; it does not
infer anything.

To make steering objectively testable anyway, `app/analysis/steering_sweep.py`
re-renders the same recording through the **unmodified** algorithm at a
sweep of candidate azimuths (via repeated `sonitude_wav_replay` calls, one
per candidate) and reports the azimuth with the highest beamformed output
energy as a measured "response peak." This is an energy-based beam-response
sweep, not an independent DOA estimate — it requires a recording with one
dominant, reasonably stationary source at a known "expected" angle to be
meaningful, and should be read as a regression check ("does the array's
response still peak near a known test source"), not a general localization
tool. When you enable "Run steering sweep for this batch" and enter an
expected direction, `metrics.json`'s `steering.estimate_available` becomes
`true`, `steering.objective_sweep_test` holds the full sweep curve and
measured/expected/error values, and the steering dial's estimated-direction
needle is drawn. Without the sweep enabled, `estimate_available` stays
`false` and the UI correctly reports the error as not computable, rather
than fabricating one.

If a real DOA estimator is ever added to the pipeline, wire its output into
`compute_steering_error`'s `estimated_events` parameter instead — that path
already exists, is unit-tested, and doesn't care whether the estimate came
from a sweep or a real algorithm.

## Architecture (module map)

```
testbench/app/
├── main.py                       GUI entrypoint
├── config_reader.py               reads capture sample rate / active_channel_map
│                                   from config/*.yaml for pre-flight validation
├── version_info.py                reads algorithm version from CMakeLists.txt
├── ui/                            PySide6 widgets only — no processing logic
│   ├── main_window.py             QTabWidget: Recorded Data / Real-Time Audio
│   ├── recorded_tab.py            Mode 1 GUI, owns a BatchWorker
│   ├── realtime_tab.py            Mode 2 GUI, owns a RealtimeWorker
│   ├── steering_dial.py           draggable compass widget (QPainter)
│   ├── playback_panel.py          RAW/PROCESSED/RESIDUAL transport (QMediaPlayer)
│   ├── visualization_panel.py     waveform/spectrogram/levels (pyqtgraph)
│   └── metrics_panel.py           renders metrics.json with method labels
├── controller/                    GUI ↔ processing glue, all QThread-based
│   ├── batch_controller.py        BatchWorker: runs Mode 1 off the GUI thread
│   ├── realtime_controller.py     RealtimeWorker: runs Mode 2 off the GUI thread
│   └── test_session.py            plain dataclasses, no Qt
├── audio_io/
│   ├── wav_loader.py               validation + metadata (soundfile)
│   ├── device_manager.py           sounddevice device enumeration
│   ├── playback_engine.py          QMediaPlayer wrapper
│   └── downmix.py                  playback-only stereo previews (not the algorithm)
├── processing/                     the ONLY files that call the C++ tools
│   ├── sonitude_binary_locator.py  finds built CLI tools
│   ├── batch_adapter.py            wraps sonitude_wav_replay
│   └── stream_adapter.py           wraps sonitude_stream_process (binary framing)
├── analysis/                       pure Python metrics on numpy arrays
│   ├── residual.py
│   ├── noise_suppression.py
│   ├── sii.py                      internal name kept; displayed as "intelligibility proxy"
│   ├── steering_error.py
│   └── steering_sweep.py           objective steering test (energy-peak sweep)
└── storage/
    ├── models.py                   dataclasses (WavMetadata, TestPaths, ...)
    └── result_store.py             TEST_xxx/ layout, metadata.json, metrics.json
```

## Extending this test bench

- **New batch metric:** add a function to `app/analysis/`, call it from
  `BatchWorker._process_one` in `app/controller/batch_controller.py`, add it
  to the `metrics` dict, and render it in `app/ui/metrics_panel.py`.
- **New processing algorithm/variant:** add a new adapter in
  `app/processing/` following `batch_adapter.py`'s pattern (build a command
  line, run it, interpret the output files) — the GUI/controller layers
  don't need to know the difference.
- **New visualization:** add a method to `VisualizationPanel`
  (`app/ui/visualization_panel.py`); it's a plain pyqtgraph wrapper, not
  coupled to Mode 1 or Mode 2.

## Known limitations

- C++ additions are not compile-verified in this environment (see
  "Build status disclosure" above) — build and test them before relying on
  this app. The two integration test files will catch real problems as soon
  as a toolchain is available.
- Real-time mode's block-per-request-response subprocess protocol adds
  latency (at least one round trip per block); this is a test bench for
  *correctness*, not a low-latency path, per this project's own scope
  guardrails (`docs/CodebaseState.md` SCOPE-1/SCOPE-4).
- The intelligibility proxy is an approximation, not a certified
  implementation — see above.
- Steering's "estimated direction" only becomes available when the optional
  sweep test is run; it's an energy-peak beam-response measurement against
  the unmodified algorithm, not an independent DOA estimate — see "Steering"
  above for what it does and doesn't prove.
- "Steering width" is a test-bench-defined directivity blend, not a native
  beamformer capability — see "Steering width definition" above.
- The steering dial, width slider, and suppression toggle apply per-batch in
  Mode 1 (one setting for every file in a run), not per-file. Per-file
  overrides (e.g. a CSV of expected azimuth per input file) would be a
  reasonable follow-up if batches need mixed test conditions.
- Real-time recording writes two temp WAV files per session (raw + processed)
  that aren't cleaned up automatically if the app crashes before Save; check
  your OS temp directory if disk usage from failed sessions becomes an issue.
