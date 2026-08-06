# Sonitude Real-Time Audio

Sonitude is a staged Raspberry Pi 5 real-time audio engineering proof-of-concept for a six-microphone head-worn array. The v1 objective is deterministic directional listening with ODAS-driven control and a custom low-latency time-domain beamforming audio path.

This repository currently implements **Milestone 0 (scaffold only)**.

## Current status

- Implemented: project scaffold, typed YAML config loading/validation, startup logging setup, base audio types, unit tests, and milestone tracking docs.
- Not implemented yet: ALSA capture/playback paths, ASRC runtime control, calibration processing, beamforming, ODAS control integration, state machine, suppression mode, and hardware measurements.

## Repository layout

```text
.
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
├── config/
├── docs/
├── scripts/
├── src/
└── tests/
```

## Dependencies

### Linux (Raspberry Pi target host)

- `cmake` (>= 3.20)
- `ninja-build`
- C++20 compiler (`g++` or `clang++`)
- `yaml-cpp`
- `spdlog`

Example install (Debian/Raspberry Pi OS):

```bash
sudo apt update
sudo apt install -y cmake ninja-build g++ libyaml-cpp-dev libspdlog-dev
```

### Windows development scaffold validation

Milestone 0 can be configured and tested on Windows if CMake and a C++ toolchain are available. If `yaml-cpp`/`spdlog` are not installed, CMake fetches them automatically when `SONITUDE_FETCH_DEPS=ON`.

## Build

From repository root:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Or using presets:

```bash
cmake --preset default-debug
cmake --build --preset build-debug
```

## Run

Validate config only:

```bash
./build/sonitude_realtime --config config/default.yaml --validate-config
```

Run scaffold executable:

```bash
./build/sonitude_realtime --config config/default.yaml
```

Expected output includes:

- successful config validation
- explicit Milestone 0 message that realtime audio is not yet implemented

## Tests

```bash
ctest --test-dir build --output-on-failure
```

## Device and milestone documentation

- Architecture and thread/data ownership: [`docs/architecture.md`](docs/architecture.md)
- Linux/ALSA setup and runtime policy: [`docs/device_setup.md`](docs/device_setup.md)
- Calibration format and tooling roadmap: [`docs/calibration.md`](docs/calibration.md)
- Latency measurement method and caveats: [`docs/latency_measurement.md`](docs/latency_measurement.md)
- Full milestone gate checklist and evidence tracking: [`docs/milestones.md`](docs/milestones.md)

## Scope guardrails

- No desktop audio servers in the critical path (PipeWire/PulseAudio/JACK).
- No ODAS audio processing in the critical path (ODAS is control-only).
- No MVDR/LCMV/GSS/neural DSP in v1 baseline milestones.
- No unmeasured end-to-end latency claims.
