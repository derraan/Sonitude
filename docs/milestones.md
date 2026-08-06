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
  - Hardware sweep/impulse capture validation pending Pi execution.

## Milestone 4 - Beamformer

- Status: `pending`
- Gate:
  - deterministic fractional delay-and-sum implementation
  - scripted steering WAV harness passes synthetic alignment checks

## Milestone 5 - ODAS control integration

- Status: `pending`
- Gate:
  - mock provider and ODAS adapter operational
  - safe fallback on ODAS loss verified

## Milestone 6 - Conversation state machine

- Status: `pending`
- Gate:
  - deterministic hysteresis transitions validated by tests
  - telemetry visibility for state and confidence

## Milestone 7 - Suppression v1

- Status: `pending`
- Gate:
  - one-distractor conservative suppression policy integrated
  - smooth fade in/out and safe fallback verified

## Milestone 8 - Measurement and hardening

- Status: `pending`
- Gate:
  - latency marker tooling and soak logs implemented
  - measured latency percentiles captured and reported
