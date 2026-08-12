# Sonitude Real-Time Audio

Sonitude is a staged Raspberry Pi 5 real-time audio engineering proof-of-concept for a six-microphone head-worn array. The v1 objective is deterministic directional listening with ODAS-driven control and a custom low-latency time-domain beamforming audio path.

## Pipeline

Sonitude splits into a **real-time audio path** (PCM) and a **control path** (steering only). ODAS is **control-only** in v1 — it does not process audio. Dashed edges are non-RT or planned stages.

```mermaid
flowchart TB
    subgraph hw [Hardware]
        Pico["Pico UAC2\n8ch container / 6 active"]
        DAC["Creative USB DAC"]
    end

    subgraph capture [Capture — RT thread]
        AlsaCap["ALSA capture worker"]
        Extract["Channel extract\nactive_channel_map"]
        Cal["CalibrationApplier\npolarity / gain / DC / HP"]
    end

    subgraph audio [Audio path — RT]
        Passthrough["Passthrough tap\nmics 4 and 5 → L/R"]
        BF["Delay-and-sum beamformer\nfractional delays + crossfade"]
        Suppress["Suppression v1\nM7 planned"]
        Limiter["Limiter\nplanned"]
        MonoStereo["Mono → duplicate stereo"]
        Asrc["ASRC: PI controller +\nIStereoResampler"]
        AlsaPb["ALSA playback worker"]
    end

    subgraph control [Control path — non-RT]
        Odas["ODAS or MockDoaProvider"]
        Parser["SST parser / source tracker"]
        SM["Conversation state machine\n+ ZoneMap + VAD"]
        Snap["Steering snapshot\nseqlock publish"]
    end

    subgraph offline [Offline / diagnostics]
        WavReplay["sonitude_wav_replay\n6ch WAV → mono WAV"]
        Tools["probe / capture_check /\ncalibration_estimate"]
    end

    Telem["Telemetry thread\natomic counters"]

    Pico -->|"S16 interleaved"| AlsaCap
    AlsaCap --> Extract --> Cal
    Cal --> Passthrough
    Cal --> BF
    Snap -.->|"az/el + failsafe"| BF
    BF --> Suppress --> Limiter --> MonoStereo
    Passthrough --> MonoStereo
    MonoStereo --> Asrc --> AlsaPb --> DAC

    Odas --> Parser --> SM --> Snap

    Cal -.-> WavReplay
    Snap -.-> WavReplay

    AlsaCap -.-> Telem
    Asrc -.-> Telem
    SM -.-> Telem

    Pico -.-> Tools
```

| Stage | What it does |
|-------|----------------|
| **Extract** | Pull 6 active channels from 8-channel USB container (`MicFrame` = 6 floats) |
| **Calibration** | Per mic: polarity, DC subtract, gain, high-pass (`CalibrationApplier`) |
| **Beamformer** | Align mics in time for steering angle; sum with 1/6 weights → mono (M4) |
| **ASRC** | PI controller adjusts playback resample ratio so capture/playback clock drift does not XRUN |
| **Passthrough mode** | Today: ear-cup mics 4/5 to L/R, bypasses beamformer (`--mode passthrough`) |
| **Control** | ODAS/mock → tracker → state machine → atomic snapshot; audio thread reads snapshot only |

**Clock rule:** Pico and DAC clocks are independent (~tens of ppm). Never drop/duplicate samples for drift — use bounded ASRC ratio control (default ±0.5%).

Full stage-by-stage detail, implementation status, and scope vetoes: **[`docs/CodebaseState.md`](docs/CodebaseState.md)** (§1 scope, §2 DSP).

## Milestones

Authoritative gate evidence: [`docs/milestones.md`](docs/milestones.md).

| ID | Milestone | Status | Gate (summary) |
|----|-----------|--------|----------------|
| **M0** | Scaffold | `done` | CMake configure/build; `--validate-config`; CTest passes |
| **M1** | ALSA discovery and raw loopback | `in_progress` | Device probe with negotiated params; raw capture-to-output; channel order validated |
| **M2** | Real-time primitives | `in_progress` | Lock-free/preallocated path; XRUN telemetry; ASRC interface; passthrough mode |
| **M3** | Calibration and offline analysis | `in_progress` | Calibration apply path; offline estimator; YAML report with backup-safe writes |
| **M4** | Beamformer | `pending` | Fractional delay-and-sum; scripted steering WAV harness passes synthetic checks |
| **M5** | ODAS control integration | `pending` | Mock provider + ODAS adapter; safe fallback on ODAS loss |
| **M6** | Conversation state machine | `pending` | Deterministic hysteresis transitions; telemetry for state and confidence |
| **M7** | Suppression v1 | `pending` | One-distractor conservative policy; smooth fade in/out; safe fallback |
| **M8** | Measurement and hardening | `pending` | Latency marker tooling; soak logs; measured latency percentiles reported |

Status legend: `pending` · `in_progress` · `done` · `blocked`

```bash
./build/sonitude_realtime --config config/default.yaml --mode passthrough   # Linux + ALSA
ctest --test-dir build --output-on-failure                                 # portable
```

## Scope guardrails

Default v1 baseline rules. Unchecked = guardrail active; checked in [`docs/CodebaseState.md`](docs/CodebaseState.md) = user override (Cursor may implement that scope change).

| ID | Rule | Rationale |
|----|------|-----------|
| **SCOPE-1** | No desktop audio servers in the critical path (PipeWire, PulseAudio, JACK) | Adds buffering and latency variance; breaks direct-ALSA RT contract |
| **SCOPE-2** | No ODAS audio processing in the critical path — ODAS is control-only | PCM beamforming stays in Sonitude; ODAS loss must not stop audio |
| **SCOPE-3** | No MVDR / LCMV / GSS / neural DSP in v1 baseline milestones | v1 is delay-and-sum plus one conservative suppressor |
| **SCOPE-4** | No unmeasured end-to-end latency claims | Only M8 impulse/loopback measurement may support latency statements |
| **SCOPE-5** | No distance-estimation or strong automatic nulling claims; at most one suppressor in v1 | Avoids unsupported product statements and M7+ scope creep |
| **SCOPE-6** | No milestone marked complete without its observable gate (evidence in `docs/milestones.md`) | Staged delivery integrity |
| **SCOPE-7** | Pico firmware and neural/HRTF code remain out of tree — contracts only | Keeps host app repo focused; firmware stays in sibling repos |

Veto checkboxes and override log: [`docs/CodebaseState.md` §1](docs/CodebaseState.md#1-global-project-scope).

## Current status

See the [Milestones](#milestones) table above. Quick summary:

- **Implemented:** scaffold, typed config, ALSA probe/workers, RT primitives (SPSC, block pool), ASRC + resampler, calibration load/apply/WAV tools, passthrough mode, unit tests.
- **In progress / pending gates:** Pi hardware soak (M1–M3), M4 beamformer, M5 ODAS control, M6 state machine, M7 suppression, M8 latency measurement.

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

Linux passthrough (6-mic capture → ear-cup stereo → ASRC → playback):

```bash
./build/sonitude_realtime --config config/default.yaml --mode passthrough
```

Passthrough requires ALSA and configured capture/playback devices. On Windows, build and run unit tests only.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

## Device and milestone documentation

- **Codebase snapshot (scope, DSP, layout, interfaces):** [`docs/CodebaseState.md`](docs/CodebaseState.md)
- Architecture and thread/data ownership: [`docs/architecture.md`](docs/architecture.md)
- Linux/ALSA setup and runtime policy: [`docs/device_setup.md`](docs/device_setup.md)
- Calibration format and tooling roadmap: [`docs/calibration.md`](docs/calibration.md)
- Latency measurement method and caveats: [`docs/latency_measurement.md`](docs/latency_measurement.md)
- Full milestone gate checklist and evidence tracking: [`docs/milestones.md`](docs/milestones.md)
