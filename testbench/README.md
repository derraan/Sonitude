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
                                                           PeakLimiter)
```

Two C++ CLI tools are used, both built from the repo's own `CMakeLists.txt`
and part of the portable (ALSA-free) subset, so they build on any platform:

| Tool | Existing / new | Used for |
| --- | --- | --- |
| `sonitude_wav_replay` | Existing (M4), **extended** with two new optional flags | Mode 1 — batch WAV processing |
| `sonitude_stream_process` | **New** (`src/tools/stream_process.cpp`) | Mode 2 — real-time streaming |

Both wrap the *same* `CalibrationApplier → DelaySumBeamformer →
ConservativeSuppressor → PeakLimiter` chain used by `sonitude_realtime`.
Nothing about the DSP was changed; see "What changed in the C++ tree" below
for the exact diff.

> **Build status disclosure:** the environment this test bench was built in
> has no CMake or C++ compiler installed, so the C++ changes below are
> written to match the existing code's exact patterns and conventions but
> have **not been compile-verified**. Build them (see "Building the C++
> tools") and fix anything the compiler flags before relying on this app.
> The Python side (everything under `testbench/`) **has** been installed,
> unit-tested, and smoke-tested end-to-end, including with a real audio
> device list from this machine.

## What changed in the C++ tree

- `src/tools/wav_replay.cpp` — added optional `--output-beamformed` and
  `--output-suppressed` flags that dump the mono signal immediately after
  the beamformer and immediately after suppression (both before the final
  `--output`, which is post-limiter). These are diagnostic taps only; the
  final render is unchanged. Needed because residual/noise metrics require
  same-domain, same-alignment signal pairs (see "Residual definition"
  below) that the tool didn't expose before.
- `src/tools/stream_process.cpp` — **new** file. A block-streaming adapter
  around the same DSP chain: reads framed 6-channel PCM blocks from stdin,
  runs them through calibration → beamformer → suppressor → limiter, writes
  framed stereo PCM blocks to stdout. No ALSA, no device I/O — the Python
  side owns the microphone/speaker via `sounddevice` and pipes blocks
  through this process. Wire protocol is documented in the file's header
  comment. This is the file to review most carefully since it's new, not an
  extension of existing code.
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
suppression metrics, approximate SII, steering script parsing, and result
storage — 30 tests, no C++ binary or audio hardware required. GUI widget
construction was additionally smoke-tested manually with a real synthetic
6-channel WAV and `QT_QPA_PLATFORM=offscreen` (not part of the automated
suite, since it needs a Qt platform plugin).

## Mode 1 — Recorded Data

Select a WAV file or a folder of WAV files. Each is validated (≥6 channels,
exact sample-rate match to `config/default.yaml`'s `capture.sample_rate_hz`
— the pipeline does no resampling), then processed by `sonitude_wav_replay`
on a background `QThread` (`BatchWorker`) so the GUI stays responsive.

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

Pick an input device with ≥6 channels, hit START. `RealtimeWorker` (a
`QThread`) captures blocks via `sounddevice`, sends each one to a live
`sonitude_stream_process` subprocess along with the current steering-dial
azimuth, plays the returned stereo block immediately, and updates level
meters and a live waveform. RECORD buffers raw and processed blocks in
memory; STOP + "Save Raw/Processed Recording" writes them to WAV.

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

### Approximate SII (`sii` in metrics.json)

**Not a certified ANSI/ASA S3.5-1997 implementation.** It sums
per-band importance weights times a per-band audibility function derived
from a simplified SNR model. The band importance weights in
`app/analysis/sii.py` are illustrative (shaped like the standard's
importance function, not transcribed from it) and are documented as such in
that file. Useful for relative before/after comparison on the same signal;
**do not use for certified, regulatory, or clinical claims.**

### Steering (`steering` in metrics.json)

The Sonitude algorithm today only *consumes* a commanded steering target
(a script in batch mode, the dial in real-time mode) — nothing in the
pipeline currently *estimates* a direction from the signal. The mock DOA
provider replays scripted values as if they were ground truth; it does not
infer anything. So `metrics.json`'s `steering.estimate_available` is always
`false` today, and the GUI's steering panel shows only the commanded
direction. If a DOA estimator is ever added to the pipeline, wire its output
into `app/analysis/steering_error.compute_steering_error`'s
`estimated_events` parameter — the error-computation path already exists
and is unit-tested, it just has nothing to consume yet.

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
│   ├── sii.py
│   └── steering_error.py
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
  this app.
- Real-time mode's block-per-request-response subprocess protocol adds
  latency (at least one round trip per block); this is a test bench for
  *correctness*, not a low-latency path, per this project's own scope
  guardrails (`docs/CodebaseState.md` SCOPE-1/SCOPE-4).
- SII is an approximation, not a certified implementation — see above.
- Steering "estimated direction" is not available until a DOA estimator
  exists in the pipeline — the UI and metrics correctly report this as
  unavailable rather than fabricating a value.
