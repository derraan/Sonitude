# Sonitude Audio Algorithm Test Bench

A PySide6 GUI for testing the existing Sonitude beamforming / suppression
chain against pre-recorded six-microphone audio or a live six-channel
device, **without reimplementing production DSP in Python**. This is a
development/test tool, not a production deployment path and not a
low-latency substitute for `sonitude_realtime`.

## How the algorithm is reused

```
PySide6 GUI  →  Controller (QThread)  →  subprocess  →  C++ CLI  →  sonitude_core DSP
```

| Tool | Role |
| --- | --- |
| `sonitude_wav_replay` | Mode 1 batch: decoded 6-channel WAV + steering script → mono + diagnostic taps |
| `sonitude_stream_process` | Mode 2 live: protocol v2 stdin/stdout blocks around the same DSP chain |

The C++ chain is `CalibrationApplier → DelaySumBeamformer →
ConservativeSuppressor → PeakLimiter`, plus optional `BinauralRenderer` in
the portable CLI tools (not yet in `sonitude_realtime`). Python never
implements HRTF/ITD. Binaural controls are **capability-gated** from
`--capabilities`. This tree implements `mono_reference`, `itd_ild`,
`compact_hrtf`, and `full_hrtf_reference`. Rebuild the CLI tools after
checkout; an older binary that only advertises `mono_reference` still
gates HRTF. `--output` stays mono; listen to the **Binaural** DSP stage.

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

Binaural input flags: bit1 enabled, bit2 follow steering. Output flags:
bit1 applied, bit2 unavailable, bit3 mono_reference. Backend byte:
`0` none, `1` mono_reference, `2` itd_ild, `3` compact_hrtf,
`4` full_hrtf_reference.

### Binaural renderer (C++ DSP)

The GUI enables C++ rendering through `--output-binaural` / stream flags
and follow/fixed-direction CLI overrides. Defaults to `compact_hrtf` when
that backend is advertised. Tables live under
`data/hrtf/generic_sadie2_d2/`. Notes: `docs/binaural_renderer.md`.

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
│   ├── binaural_controls.py
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
  `--output-binaural` (`mono_reference`, `itd_ild`, `compact_hrtf`,
  `full_hrtf_reference`) plus `--binaural-follow-steering` /
  `--binaural-fixed-direction` / `--binaural-azimuth` /
  `--binaural-elevation`
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
- GUI must not pretend a backend works if `--capabilities` does not list it
  or if an HRTF table fails to load (`BINAURAL_UNAVAILABLE`).
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
