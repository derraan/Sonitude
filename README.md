# Sonitude

Six-microphone directional-audio proof of concept: a C++20 DSP/runtime, a PySide6 engineering test bench, calibration tools, a Pico USB microphone firmware snapshot, and a separate Sound Bubble neural-audio research tree.

**Status:** implemented host software with automated tests; hardware acceptance and measured end-to-end latency remain unfinished. This is not a deployment-approved headset release. The overview below was checked against main commit `c096136` on 2026-09-10; implementation and hardware evidence are distinguished throughout.

## Start here

| Area | Entry point | Purpose |
| --- | --- | --- |
| Host DSP and Linux runtime | [src](src), [CMakeLists.txt](CMakeLists.txt) | Six-channel calibration, MVDR, suppression, binaural rendering, ALSA and ASRC |
| Desktop test bench | [testbench](testbench/README.md) | Recorded-file processing, live device preview, calibration and configuration upload |
| Calibration | [guide](docs/calibration.md), [tools/calibration](tools/calibration) | Scalar channel calibration and measured complex-RTF array profiles |
| Pico firmware | [firmware README](mic-array-pico2w-usb6ch/README.md) | Separate USB microphone capture project; not built by the host CMake project |
| Neural research/runtime | [Sound_Bubble](Sound_Bubble/README.md), [edge runtime](Sound_Bubble/edge/README_EDGE.md) | Separate training/export/inference workflow; not part of the C++ MVDR chain |
| Acceptance tracking | [milestones](docs/milestones.md) | Outstanding software/hardware gates and evidence templates |

## Current processing paths

### Linux runtime

`sonitude_realtime` runs capture and DSP on the main audio thread, with separate playback, control and telemetry threads. Capture channel extraction uses `active_channel_map`. Playback uses a bounded block pool and ASRC to accommodate capture/playback clock drift.

The ordinary `adaptive_geometric` beamform path is:

1. Capture and channel mapping.
2. Per-channel DC-offset subtraction, polarity, gain and DC blocking.
3. Optional per-microphone biquad EQ.
4. Adaptive near-field MVDR.
5. Optional suppression (**Wiener spectral in the M7 pipeline**, or conservative PCM), common biquad EQ and mono peak limiting.
6. Binaural rendering, linked stereo peak limiting, then playback resampling/ALSA.

Binaural rendering **is wired into the Linux runtime**. The former duplicated-mono output fallback is rejected in the ordinary beamform path. The configured renderer must initialize successfully.

Other paths have different semantics:

| Path | Behavior and limitation |
| --- | --- |
| `--mode passthrough` | Calibrated/EQ'd logical channels 4 and 5 feed L/R; bypasses beamforming, suppression and the beamform limiter chain. These indices are hardcoded, not selected through the steering ear-index settings. |
| `spatial.backend: adaptive_geometric` | Adaptive geometric MVDR with near-field steering. Current default configuration uses a 0.45 m assumed source distance; this is a setting, not an estimated source distance. |
| `spatial.backend: fixed_measured` | Loads an SMV3 array profile and emits stereo through `FixedBinauralMvdr`; requires a separately compiled profile. |
| `steering.experimental_dual_reference_mvdr: true` | Experimental direct stereo MVDR path, bypassing the ordinary HRTF renderer. |
| Direct stereo paths | See the unresolved suppression/EQ routing defect below before evaluating these paths. |

Steering convention: azimuth 0° is front (+Y), positive toward listener-right (+X); see [head_frame.hpp](src/spatial/head_frame.hpp). Confirm physical channel order and geometry on the actual array.

### Test bench

The four tabs cover **Recorded**, **Real-Time**, **Calibration** and **Upload**. Python manages UI, files and audio devices; subprocesses invoke the C++ DSP through `sonitude_wav_replay` and `sonitude_stream_process`.

- WAV/FLAC input and output; MP3 input depends on decoder support. MP3 export is not supported.
- Decoding a file does not make its channel count suitable for the six-microphone pipeline.
- Recorded processing supports short-file diagnostic taps and a streaming path for longer files.
- The streaming protocol and executable capabilities must match the Python app; rebuild tools after updating.
- Directional/Omni Blend is a test-bench mixing control, not a measured beamwidth.
- The intelligibility metric is an experimental proxy, not standardized SII or an acceptance gate.
- Desktop live preview is a development path, not evidence of production latency.

See [testbench/README.md](testbench/README.md) for operation and [binaural renderer](docs/binaural_renderer.md) for rendering details. Some detailed documents retain older behavior descriptions; compare them with current source and executable capabilities.

## Build and run

### Dependencies

Host build: **CMake 3.25+**, a C++20 compiler, Ninja (for commands below), yaml-cpp and spdlog. Linux device runtime also requires ALSA development files. libsamplerate is optional; the build has a linear fallback.

Example Debian/Raspberry Pi OS dependency installation:

```bash
sudo apt update
sudo apt install -y cmake ninja-build g++ pkg-config libasound2-dev libyaml-cpp-dev libspdlog-dev libsamplerate0-dev
cmake --version
```

Ensure the installed CMake meets the minimum before configuring. CMake can fetch missing supported dependencies with `SONITUDE_FETCH_DEPS=ON`; the following reproducible build instead requires them locally:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONITUDE_FETCH_DEPS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Portable/offline build, without Linux device runtime:

```bash
cmake -S . -B build-portable -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONITUDE_WITH_ALSA=OFF -DSONITUDE_WITH_LIBSAMPLERATE=OFF
cmake --build build-portable --parallel
ctest --test-dir build-portable --output-on-failure
```

With ALSA disabled, `sonitude_realtime` and the ALSA device diagnostics are **not built**. Portable replay/stream tools and unit tests remain available.

Presets are also provided:

```bash
cmake --preset default-debug
cmake --build --preset build-debug
ctest --test-dir build/debug --output-on-failure
```

### Linux hardware runtime

Inspect and edit [config/default.yaml](config/default.yaml) for the connected devices, sample rate, channel map, geometry and calibration before running.

```bash
./build/sonitude_realtime --config config/default.yaml --validate-config
./build/sonitude_device_probe --config config/default.yaml
./build/sonitude_realtime --config config/default.yaml --mode passthrough
# Or run directional processing:
./build/sonitude_realtime --config config/default.yaml --mode beamform
```

`--validate-config` checks runtime configuration and geometry, then returns before device opening, calibration loading or DSP/profile initialization. It is not a complete readiness check.

The checked-in launcher only selects passthrough. It is stored without the executable bit; invoke it with Bash:

```bash
bash scripts/run_realtime.sh config/default.yaml
```

Current default settings:

| Setting | Checked-in value |
| --- | --- |
| Capture / playback devices | `hw:PicoMic,0` / `hw:Creative,0` |
| Nominal sample rates / periods | 44,100 Hz / 64 frames / 3 periods |
| Logical channel map | `[0, 1, 2, 3, 4, 5]` |
| Calibration | `calibration_uploaded.yaml` |
| Spatial backend | `adaptive_geometric` |
| Steering model / assumed range | `near_field` / 0.45 m |
| Suppression | Disabled; selected backend is `spectral` |
| Binaural | Enabled, `compact_hrtf`, follows steering |
| ODAS | Disabled; no live direction tracking by default |
| Common EQ | Enabled flag, empty section list: no filter applied |

These are configuration values, not validated hardware specifications. In particular, confirm channel mapping and the provenance/suitability of the uploaded calibration. The compact HRTF data remains an experimental raw-HRIR-prefix representation, not a validated embedded deployment profile.

### Desktop test bench

After building the portable tools, from the repository root on Linux/macOS:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r testbench/requirements-dev.txt
export SONITUDE_BUILD_DIR="$PWD/build"
export SONITUDE_REQUIRE_CPP=1
cd testbench
python -m pytest -q
python -m app.main
```

Point `SONITUDE_BUILD_DIR` at the directory actually built, such as `build-portable` or `build/debug`. On Windows, activate with `.venv\Scripts\activate` and set the environment variables using your shell's syntax. For headless tests, set `QT_QPA_PLATFORM=offscreen`.

Without `SONITUDE_REQUIRE_CPP=1`, missing C++ binaries can cause integration tests to skip.

## Calibration workflows

| Workflow | Entry point | Output/use |
| --- | --- | --- |
| Channel diagnostics | `sonitude_calibration_capture`, `sonitude_calibration_estimate` | Synthetic/offline diagnostics and scalar calibration YAML |
| Python scalar compiler | `python -m tools.calibration`, Calibration tab | Delay/gain/polarity variants A–E and reports; runtime overlays |
| Measured array compiler | `tools/calibration/compile_array.py` | SMV3 profile plus NPZ/CSV/JSON diagnostics for `fixed_measured` |

The measured compiler accepts synchronized six-channel sweeps with a known stimulus, or synchronized exported IR WAV/FLAC files. See [calibration guide](docs/calibration.md) and [example manifest](config/array_ir_manifest.example.yaml). Replace the example's placeholder recording paths with actual data before running:

```bash
python tools/calibration/compile_array.py --manifest config/array_ir_manifest.example.yaml --output-prefix build/array_profile_measured
```

Set `spatial.backend: fixed_measured` and `spatial.profile_path` to the generated binary to select that path. The checked-in default does not select it.

Scalar `delay_samples` is consumed by geometric steering, not the time-domain `CalibrationApplier`. Measured profiles encode synchronized delay in complex IR/RTF phase; the array compiler deliberately ignores scalar delay corrections. Keep runtime gain/polarity/EQ and profile-conditioning assumptions consistent.

REW `.mdat` import is metadata support, not general IR extraction. A compiled artifact or single-direction recording does not establish full measured azimuth coverage or hardware acceptance.

## Unfinished work and known issues

Audit basis: source review at `c096136`, repository workflows and open PR metadata, 2026-09-10. These findings have not been reproduced on audio hardware.

| Priority | Finding / remaining action | Evidence |
| --- | --- | --- |
| High: RT liveness risk | `SnapshotBuffer::acquire()` spins without a bound while a publication is in progress. If a higher-priority FIFO audio reader preempts the lower-priority writer on the same CPU with an odd sequence, the reader can prevent that writer from completing. Replace with bounded last-good-snapshot behavior or a suitable handoff and validate under contention. This is a source-derived scheduling risk, not a reproduced hang. | [snapshot](src/rt/param_snapshot.hpp), [runtime](src/main.cpp) |
| High: output correctness | Fixed-measured and experimental dual-reference stereo paths process suppression on a temporary mono average, but output the original stereo buffers. Common mono EQ and test-bench blend likewise do not affect that stereo output. Streaming can still set its suppression-applied flag. Route intended processing into the audible stereo path and add output-level regression coverage. The fixed MVDR's internal mask is separate from this post-suppression defect. | [runtime](src/main.cpp), [stream](src/tools/stream_process.cpp), [replay](src/tools/wav_replay.cpp) |
| High: failure reporting | Runtime capture-wait and repeated playback/queue failures stop the loop but reach `return 0`. Propagate a failure exit status and verify process-supervisor behavior. | [runtime](src/main.cpp) |
| High: RT contract | Failed memory locking and FIFO scheduling only warn and continue; playback starts before its scheduling call. Complete the required/degraded-mode policy and scheduling acceptance checks. | [runtime](src/main.cpp), [draft PR #28](https://github.com/derraan/Sonitude/pull/28) |
| Medium: channel consistency | Passthrough hardcodes channels 4/5, while default steering ear references are 0/5. Resolve against measured wiring/geometry; do not assume both identify the same ear pair. | [runtime](src/main.cpp), [default config](config/default.yaml) |
| Medium: operational checks | Launcher lacks executable permission; direct execution fails on Unix. Use Bash as shown above. Config-only validation does not load all runtime assets. | [launcher](scripts/run_realtime.sh), [runtime](src/main.cpp) |
| Pending validation | Device format/channel mapping, accepted calibration, XRUN/occupancy soaks, shutdown/fault behavior, ODAS loss, steering/suppression transitions and measured latency percentiles. | [milestones](docs/milestones.md) |
| Pending integration | Production VAD is absent from the control decision: ODAS confidence is used as a speech-probability proxy. Own-voice foundation remains in a draft PR. | [control loop](src/control/control_loop.cpp), [draft PR #29](https://github.com/derraan/Sonitude/pull/29) |
| Documentation | Detailed milestone and architecture prose still contains older runtime descriptions. Reconcile these with implementation without marking hardware gates complete. | [milestones](docs/milestones.md), [architecture](docs/architecture.md), [codebase state](docs/CodebaseState.md) |

### Open work on GitHub

At the audit time, there were **two open draft PRs and no open standalone issues**:

- [#28 — Integrated release candidate: thread safety, control, evidence, and CI](https://github.com/derraan/Sonitude/pull/28), targeting `main`. Its stated outstanding gates include Pi scheduling/memory-lock evidence, soaks, teardown/fault tests and hardware calibration. Reconcile with current main before integration; PR-body historical test results are not proof of current-main acceptance.
- [#29 — Add own-voice subsystem foundation](https://github.com/derraan/Sonitude/pull/29), targeting `integration/release-candidate-2026-08-14`, not main. Runtime wiring, measured calibration/thresholds and hardware acceptance remain unfinished in its stated scope.

The issue tracker therefore does not represent the complete backlog. The table above and [milestone tracker](docs/milestones.md) contain additional work.

Milestone status recorded in that tracker: **M0 done; M1–M7 in progress; M8 pending**. Recorded status is not a fresh certification.

## Automated checks and limits

For audited main commit `c096136`:

- [Linux build run](https://github.com/derraan/Sonitude/actions/runs/34435376645): libsamplerate ON/OFF builds, ASan/UBSan, ThreadSanitizer and minimal host jobs all succeeded.
- [Gitleaks run](https://github.com/derraan/Sonitude/actions/runs/34435376859): succeeded.
- Current [host CI](.github/workflows/linux-build.yml) runs CTest and test-bench pytest with real C++ tools.
- That workflow does not build Pico firmware or run the Sound_Bubble neural workflow. Their readiness is not established by green host CI.
- The README audit did not rerun local C++ builds: the review environment lacked CMake and required development libraries. CI results above are retrieved GitHub results.

Green automated checks do not close hardware gates or disprove untested runtime routing/scheduling defects. No end-to-end latency, speech-separation performance or deployment-readiness claim is made here.

## Further documentation

- [Architecture and ownership](docs/architecture.md)
- [Codebase snapshot and scope vetoes](docs/CodebaseState.md)
- [Linux device setup](docs/device_setup.md)
- [Calibration](docs/calibration.md)
- [Binaural rendering](docs/binaural_renderer.md)
- [Wiener spectral postfilter (M7 pipeline)](docs/spectral_postfilter.md)
- [Latency measurement](docs/latency_measurement.md)
- [Milestone evidence](docs/milestones.md)
- [Test-bench usage](testbench/README.md)
