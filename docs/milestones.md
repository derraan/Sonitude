# Milestone Tracker

This file tracks execution status, evidence, and unresolved assumptions for Milestones 0-8.

## Status legend

- `pending`: not started
- `in_progress`: implementation underway
- `done`: gate met with recorded evidence
- `blocked`: cannot continue due to unresolved dependency/hardware fact

## Milestone 0 - Scaffold

- Status: `done`
- Gate:
  - CMake configure/build succeeds
  - `sonitude_realtime --validate-config` succeeds with default config
  - CTest unit suite passes
- Evidence command template:
  - `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`
  - `cmake --build build`
  - `./build/sonitude_realtime --config config/default.yaml --validate-config`
  - `ctest --test-dir build --output-on-failure`
- Evidence/result:
  - `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug` completed successfully.
  - `cmake --build build` completed successfully and produced:
    - `build/sonitude_realtime.exe`
    - `build/sonitude_device_probe.exe`
    - `build/sonitude_calibration_capture.exe`
    - `build/sonitude_latency_marker.exe`
    - `build/sonitude_wav_replay.exe`
    - `build/sonitude_unit_tests.exe`
  - `./build/sonitude_realtime --config config/default.yaml --validate-config` passed.
  - `ctest --test-dir build --output-on-failure` passed (`1/1` tests).
- Unresolved assumptions:
  - Raspberry Pi 5 runtime environment not validated in Milestone 0
  - ALSA hardware contracts to be measured in Milestone 1

## Milestone 1 - ALSA discovery and raw loopback

- Status: `in_progress`
- Gate:
  - capture and playback devices probed with negotiated params logged
  - raw capture-to-output path runs without processing
  - channel activity/order validated
- Evidence command template:
  - `./build/sonitude_device_probe --config config/default.yaml`
  - `./build/sonitude_capture_check --config config/default.yaml`
  - `./build/sonitude_playback_check --config config/default.yaml`
  - `./build/sonitude_loopback_diag --config config/default.yaml`
  - `./scripts/verify_alsa_devices.sh hw:PicoMic,0 hw:Creative,0`
- Evidence/result:
  - Probe tooling and ALSA wrappers implemented and buildable.
  - Hardware negotiation/activity evidence must be collected on Pi 5 with connected devices.
- Negotiated parameters record template:
  - Capture: `rate=?, format=?, container_channels=?, period=?, buffer=?`
  - Playback: `rate=?, format=?, channels=?, period=?, buffer=?`
  - Channel activity: `ch0..ch5 active; ch6..ch7 silent`
  - Drift slope (diag): `? ppm`
- Unresolved assumptions:
  - Pico USB container format/rate variants (6xS16 vs 8x32 with 6 active)
  - Creative DAC supported formats/rates

## Milestone 2 - Real-time primitives

- Status: `in_progress`
- Gate:
  - lock-free/preallocated path in place
  - XRUN recovery telemetry active
  - ASRC interface and bypass mode compile-tested
- Evidence command template:
  - `ctest --test-dir build --output-on-failure`
  - `./scripts/run_realtime.sh config/default.yaml`
- Evidence/result:
  - RT primitives (SPSC ring, block pool, ASRC controller, resampler interfaces) and tests implemented.
  - M2 passthrough mode in `sonitude_realtime` implemented.
  - Current runtime keeps blocking capture, DSP, and playback on one audio loop; separate RT capture/render/playback workers and per-thread scheduling remain pending M2 hardening gates.
  - Hardware soak evidence (30 min occupancy/XRUN log) pending Pi execution.

## Milestone 3 - Calibration and offline analysis

- Status: `in_progress`
- Gate:
  - calibration apply path and offline estimator working
  - report + YAML output generated with backup-safe behavior
- Evidence command template:
  - `./build/sonitude_calibration_capture`
  - `./build/sonitude_calibration_estimate build/calibration_capture.wav build/calibration_estimate.yaml`
  - `ctest --test-dir build --output-on-failure`
- Evidence/result:
  - Calibration config loader, applier, backup-safe YAML writer, and deterministic unit tests implemented.
  - Offline estimator (`calibration_estimator`) estimates DC, relative gain (reference-normalized), normalized time-domain correlation lag, and polarity (or UNRESOLVED); writes v2 YAML + quality report. This is a channel diagnostic, not the measured-RTF spatial compiler.
  - Synthetic acceptance tests cover unity, known gain/delay/polarity injection, and strengthened validation.
  - Hardware sweep/impulse capture validation pending Pi execution.

## Milestone 4 - Beamformer

- Status: `in_progress`
- Gate:
  - deterministic STFT-domain MVDR implementation (delay-and-sum fallback)
  - scripted steering WAV harness passes synthetic alignment checks
- Evidence command template:
  - `ctest --test-dir build --output-on-failure`
  - `./build/sonitude_wav_replay --input <six_channel_wav> --config config/default.yaml --script <steering_csv> --output <beamformed_mono_wav>`
  - `./scripts/openmha_golden_render.sh --input6ch <six_channel_wav> --runtime config/default.yaml --steering <steering_csv> --sonitude-out <sonitude_m4.wav> --openmha-out <openmha_m4.wav> --metrics-out <m4_metrics.txt>`
- Evidence/result:
  - Beamformer unit coverage includes on-axis vs off-axis energy checks, click-free retarget checks, and calibration delay closure checks.
  - Offline `sonitude_wav_replay` beamformed rendering is implemented for scripted steering inputs.
  - openMHA golden-render comparison workflow template added at `tests/integration/openmha_m4_validation.md` with script scaffold `scripts/openmha_golden_render.sh`.
  - Hardware and openMHA parity metrics are not yet recorded; milestone remains `in_progress`.

## Milestone 5 - ODAS control integration

- Status: `in_progress`
- Gate:
  - mock provider and ODAS adapter operational
  - safe fallback on ODAS loss verified

## Milestone 6 - Conversation state machine

- Status: `in_progress`
- Gate:
  - deterministic hysteresis transitions validated by tests
  - telemetry visibility for state and confidence

## Milestone 7 - Suppression v1

- Status: `in_progress`
- Gate:
  - one-distractor conservative suppression policy integrated
  - smooth fade in/out and safe fallback verified
- Evidence command template:
  - `ctest --test-dir build --output-on-failure`
  - `./build/sonitude_wav_replay --input <six_channel_wav> --config config/default.yaml --script <steering_csv> --output <suppressed_mono_wav> --suppression on`
  - `./build/sonitude_wav_replay --input <six_channel_wav> --config config/default.yaml --script <steering_csv> --output <unsuppressed_mono_wav> --suppression off`
  - `--enable-suppression` remains an ON alias; `--disable-suppression` forces OFF even when YAML `suppression.enabled` is true.
- Evidence/result:
  - Conservative suppressor (`ConservativeSuppressor`) added with ambient-floor clamp, confidence gating, and failsafe ramp-to-unity behavior.
  - Peak limiter (`PeakLimiter`) added after suppression in the beamform path.
  - Deterministic unit tests added for suppressor gain floor/fallback and limiter ceiling/release behavior.
  - Runtime and offline wiring are implemented, but reference SNR logs and hardware transition checks are pending; milestone remains `in_progress`.

## Milestone 8 - Measurement and hardening

- Status: `pending`
- Gate:
  - latency marker tooling and soak logs implemented
  - measured latency percentiles captured and reported
