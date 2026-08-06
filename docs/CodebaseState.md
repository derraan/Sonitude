# CodebaseState

Unified tracker for Sonitude core: directory layout, milestone reality, key interfaces, config schema, tests, conventions, ODAS posture, and CMake wiring. Treat this document as the living snapshot; **`docs/milestones.md` remains authoritative for milestone gates**.

Last updated: 2026-08-06.

---

## 1. Directory tree (Sonitude core)

Excluding `.git`, `build/`, and the vendored `mic-array-pico2w-usb6ch/` Pico firmware tree.

```text
Sonitude/
├── CMakeLists.txt              # Root build: targets, options, CTest
├── CMakePresets.json           # default-debug / default-release presets
├── .clang-format
├── .gitignore / .gitattributes
├── README.md                   # Scaffold overview (status text is stale vs milestones.md)
├── cmake/
│   └── Dependencies.cmake      # ALSA find; FetchContent yaml-cpp/spdlog
├── config/
│   ├── default.yaml            # Runtime: devices, ASRC, steering, zones, SM, ODAS, telemetry
│   ├── geometry_soundbubble_initial.yaml  # 6-mic XYZ positions
│   └── calibration_example.yaml           # Per-mic polarity/gain/delay/DC
├── docs/
│   ├── milestones.md           # M0–M8 status + gates
│   ├── architecture.md         # Target pipeline / thread model
│   ├── calibration.md          # Calibration schema + planned order
│   ├── device_setup.md         # ALSA/RT runbook (planned)
│   ├── latency_measurement.md  # M8 measurement method
│   └── CodebaseState.md        # This document
├── scripts/
│   ├── run_realtime.sh
│   ├── set_realtime_environment.sh
│   └── verify_alsa_devices.sh
├── src/
│   ├── main.cpp                # sonitude_realtime: validate / passthrough
│   ├── app/                    # Config, calibration I/O, logging
│   ├── audio/                  # Types, PCM, WAV, ALSA workers
│   ├── dsp/                    # Calibration apply, resampler, ASRC
│   ├── rt/                     # SPSC ring, block pool, RT thread, telemetry
│   ├── spatial/                # SourceObservation stub type only
│   ├── vad/                    # IVad interface stub only
│   └── tools/                  # Probe/check/calibration/latency/replay CLIs
└── tests/
    ├── unit/                   # Single binary unit suite
    ├── fixtures/               # YAML fixtures for config tests
    └── integration/README.md   # Planned: beamformer / ODAS / SM harnesses
```

### `src/` file purposes

| Path | Purpose |
|------|---------|
| `src/main.cpp` | CLI: `--validate-config`, `--mode passthrough` (Linux ALSA loop earcup→stereo) |
| `src/app/config.hpp/.cpp` | Typed YAML load + validation for runtime/geometry |
| `src/app/calibration_config.hpp/.cpp` | Calibration YAML load/validate |
| `src/app/calibration_writer.hpp/.cpp` | Backup-safe calibration YAML write |
| `src/app/logging.hpp/.cpp` | Startup spdlog init |
| `src/audio/audio_types.hpp` | `MicFrame`, steering, stereo, sequence info |
| `src/audio/format_convert.hpp` | PCM encode/decode helpers |
| `src/audio/channel_extractor.hpp` | Container→6-mic extract |
| `src/audio/wav_io.hpp/.cpp` | Multichannel WAV read/write |
| `src/audio/alsa/*` | Probe, device open, capture/playback workers |
| `src/dsp/calibration_applier.*` | Polarity/gain/DC + HP (delay not applied yet) |
| `src/dsp/resampler.hpp` | `IStereoResampler` |
| `src/dsp/resampler_cubic.*` | Cubic fallback resampler |
| `src/dsp/resampler_src.cpp` | libsamplerate or cubic fallback factory |
| `src/dsp/asrc_controller.hpp` | Occupancy PI ratio controller |
| `src/rt/spsc_ring.hpp` | Lock-free SPSC ring |
| `src/rt/block_pool.hpp` | Preallocated block pool on SPSC free-list |
| `src/rt/rt_thread.*` | Thread start + RT scheduling attempt |
| `src/rt/telemetry.hpp` | Atomic XRUN/occupancy counters |
| `src/spatial/spatial_types.hpp` | `SourceObservation` only |
| `src/vad/vad_interface.hpp` | `IVad` only |
| `src/tools/device_probe.cpp` | ALSA device probe CLI |
| `src/tools/capture_check.cpp` / `playback_check.cpp` / `loopback_diag.cpp` | M1 ALSA diagnostics |
| `src/tools/calibration_capture.cpp` | Synthetic 6ch WAV (portable) |
| `src/tools/calibration_estimate.cpp` | DC/RMS→YAML estimator |
| `src/tools/latency_marker.cpp` | M8 placeholder |
| `src/tools/wav_replay.cpp` | M4 placeholder |

---

## 2. Milestone status (M0–M3)

From `docs/milestones.md`:

| Milestone | Status | Code reality |
|-----------|--------|--------------|
| **M0 Scaffold** | `done` | Config, types, CMake, CTest |
| **M1 ALSA** | `in_progress` | Probe/workers/tools built; Pi hardware evidence pending |
| **M2 RT primitives** | `in_progress` | SPSC, pool, ASRC, resampler, passthrough; soak pending |
| **M3 Calibration** | `in_progress` | Loader/applier/writer/WAV/tools/tests; HW sweep pending |
| **M4–M8** | `pending` | Config fields + stub types only |

`README.md` still says "Milestone 0 only" — treat `docs/milestones.md` as authoritative.

---

## 3. Key interfaces and types (verbatim)

### Audio types — `src/audio/audio_types.hpp`

```6:30:src/audio/audio_types.hpp
namespace sonitude::audio
{
constexpr std::size_t kMicChannels = 6;

using Sample = float;
using MicFrame = std::array<Sample, kMicChannels>;

struct CaptureSequenceInfo
{
  std::uint64_t sequence = 0;
  std::uint64_t capture_timestamp_ns = 0;
};

struct BeamformerSteering
{
  float azimuth_deg = 0.0F;
  float elevation_deg = 0.0F;
};

struct StereoFrame
{
  Sample left = 0.0F;
  Sample right = 0.0F;
};
}  // namespace sonitude::audio
```

No audio **block** type beyond `std::vector<MicFrame>` used by workers. `BeamformerSteering` exists; no beamformer class.

### Config structs — `src/app/config.hpp`

```10:101:src/app/config.hpp
struct ZoneConfig
{
  std::string name;
  float azimuth_min_deg = 0.0F;
  float azimuth_max_deg = 0.0F;
};
// ... DeviceConfig, AsrcConfig, GeometryMic, GeometryConfig ...
struct SteeringConfig
{
  float speed_of_sound_mps = 343.0F;
  std::size_t reference_mic_index = 0;
  float steering_ramp_ms = 150.0F;
  float ambient_floor_linear = 0.25F;
};

struct StateMachineConfig
{
  std::uint32_t activation_hold_ms = 400;
  std::uint32_t confirmation_hold_ms = 250;
  std::uint32_t release_hold_ms = 1500;
  std::uint32_t hold_direction_ms = 1000;
  float zone_direction_stability_deg = 15.0F;
};

struct OdasConfig
{
  bool enabled = false;
  bool use_mock_provider = true;
  std::string endpoint;
};
// ... TelemetryConfig ...
struct RuntimeConfig
{
  DeviceConfig capture;
  DeviceConfig playback;
  std::vector<std::size_t> active_channel_map;
  std::string geometry_path;
  std::string calibration_path;
  AsrcConfig asrc;
  SteeringConfig steering;
  StateMachineConfig state_machine;
  OdasConfig odas;
  TelemetryConfig telemetry;
  std::vector<ZoneConfig> zones;
};

RuntimeConfig LoadRuntimeConfigFromFile(const std::string& path);
GeometryConfig LoadGeometryFromFile(const std::string& path);
void ValidateRuntimeConfig(const RuntimeConfig& config);
void ValidateGeometryConfig(const GeometryConfig& geometry);
```

Steering / zones / SM / ODAS fields are **fully parsed and validated**; no runtime consumers yet.

### Calibration — config + apply

```9:28:src/app/calibration_config.hpp
struct CalibrationChannel
{
  std::string id;
  int polarity = 1;
  float gain_linear = 1.0F;
  float delay_samples = 0.0F;
  float dc_offset = 0.0F;
};
// ...
CalibrationConfig LoadCalibrationFromFile(const std::string& path);
void ValidateCalibrationConfig(const CalibrationConfig& calibration,
                               const std::vector<std::string>& geometry_ids,
                               std::uint32_t expected_sample_rate_hz);
```

```12:24:src/dsp/calibration_applier.hpp
class CalibrationApplier
{
 public:
  explicit CalibrationApplier(const std::vector<app::CalibrationChannel>& channels);
  audio::MicFrame process(const audio::MicFrame& in);
  // ...
};
```

`process()` applies polarity → gain → DC-subtract → 1-pole HP (`hp_a = 0.995`). **`delay_samples` is stored in YAML but not applied** (M4 fractional delay may own that).

Writer:

```9:12:src/app/calibration_writer.hpp
void WriteCalibrationYamlBackupSafe(const std::string& path,
                                    const CalibrationConfig& calibration,
                                    bool force_overwrite);
```

### Resampler / ASRC

```21:35:src/dsp/resampler.hpp
class IStereoResampler
{
 public:
  virtual ~IStereoResampler() = default;
  virtual void reset() = 0;
  virtual ResamplerResult process(const StereoSample* input,
                                  std::size_t input_samples,
                                  StereoSample* output,
                                  std::size_t max_output_samples,
                                  double ratio) = 0;
};

std::unique_ptr<IStereoResampler> CreateCubicResampler();
std::unique_ptr<IStereoResampler> CreateSrcResampler();
```

```7:33:src/dsp/asrc_controller.hpp
class AsrcController
{
 public:
  struct Config { /* min/max_ratio, kp, ki, target_buffer_frames, max_ratio_step */ };
  explicit AsrcController(Config cfg) : cfg_(cfg) {}
  double update(const double occupancy_frames);
  void reset();
  // ...
};
```

`SONITUDE_WITH_LIBSAMPLERATE_ENABLED` is forced to `0` in CMake; `CreateSrcResampler()` falls back to cubic.

### SPSC / pool / telemetry

```21:44:src/rt/spsc_ring.hpp
  bool push(const T& value)
  {
    // ... full-check via head/tail atomics ...
    storage_[head] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool pop(T& out)
  {
    // ... empty-check ...
    out = storage_[tail];
    tail_.store((tail + 1U) & mask_, std::memory_order_release);
    return true;
  }
```

```24:48:src/rt/block_pool.hpp
  T* acquire();
  bool release(T* ptr);
```

```8:18:src/rt/telemetry.hpp
struct TelemetryCounters
{
  std::atomic<std::uint64_t> capture_frames{0};
  // ... playback_frames, capture/playback_xruns, ring_over/underruns,
  // asrc_ratio_ppm, ring_occupancy_frames
};
```

### Missing for M4–M6 (explicitly absent)

- No fanout / lock-free multicast
- No atomic / double-buffer **steering snapshot** helper
- No `IDoaProvider` / `IBeamformer` / state-machine classes
- No shared test signal-generator library (sine is inlined in `calibration_capture.cpp`)

### WAV I/O

```12:21:src/audio/wav_io.hpp
struct WavData
{
  std::uint32_t sample_rate_hz = 0;
  std::uint16_t channels = 0;
  PcmFormat format = PcmFormat::FLOAT32_LE;
  std::vector<float> interleaved;
};

void WriteWavFile(const std::string& path, const WavData& data);
WavData ReadWavFile(const std::string& path);
```

Supports multichannel; tests round-trip 6ch S16.

### Spatial / VAD stubs (M5/M6 hooks)

```7:14:src/spatial/spatial_types.hpp
struct SourceObservation
{
  std::uint64_t source_id = 0;
  float azimuth_deg = 0.0F;
  float elevation_deg = 0.0F;
  float confidence = 0.0F;
  std::uint64_t timestamp_ns = 0;
};
```

```7:12:src/vad/vad_interface.hpp
class IVad
{
public:
  virtual ~IVad() = default;
  virtual float EstimateSpeechProbability(std::span<const float> mono_frame) = 0;
};
```

---

## 4. Geometry format

`config/geometry_soundbubble_initial.yaml`:

```yaml
profile_name: "soundbubble_initial_planar_v1"
microphones:
  - id: M0_upper_inner_left
    x: -0.0380
    y: 0.1680
    z: 0.0000
  # ... M1–M5 ...
```

- Exactly **6** mics; unique string `id`s.
- Fields: `id`, `x`, `y`, `z` (`double`).
- **Units not stated in YAML**; implied **metres** by `steering.speed_of_sound_mps` and values ~0.04–0.17 (head-scale). Docs call it "provisional planar data".
- IDs match calibration example (`M0_upper_inner_left` … `M5_right_earcup`).
- Coordinate frame: planar `z=0`; Y forward-ish (earcup at y=0, upper mics at y>0); X left/right.

---

## 5. Relevant `config/default.yaml` fields

| Section | Key values |
|---------|------------|
| **steering** | `speed_of_sound_mps: 343.0`, `reference_mic_index: 0`, `steering_ramp_ms: 150.0` (smoothing), `ambient_floor_linear: 0.25` (**suppression floor**) |
| **state_machine** | `activation_hold_ms: 400`, `confirmation_hold_ms: 250`, `release_hold_ms: 1500`, `hold_direction_ms: 1000`, `zone_direction_stability_deg: 15.0` |
| **zones** | `front_auto_focus` [-45,45], `right_assist` [45,100], `left_assist` [-100,-45], `rear_ambient` [100,260] |
| **odas** | `enabled: false`, `use_mock_provider: true`, `endpoint: "unix:///tmp/odas.sock"` |
| **telemetry** | human-readable on; CSV/JSON off; `stats_period_ms: 1000` |

Validation bounds of note: `steering_ramp_ms` ∈ [10, 500], `ambient_floor_linear` ∈ [0, 1], zone azimuth ∈ [-180, 360].

---

## 6. Tests

**Structure:** one executable `sonitude_unit_tests`; `config_tests.cpp` owns `main()` and calls:

- `RunAudioSupportTests()` — S16 round-trip, channel extract, 6ch WAV
- `RunRtPrimitiveTests()` — ring push/pop, block pool
- `RunAsrcSimulationTests()` — ±20/55/150 ppm occupancy sims
- `RunCalibrationTests()` — apply polarity/gain, backup-safe writer

**Fixtures:** `tests/fixtures/{runtime_valid,runtime_invalid_duplicate_channel,geometry_valid,geometry_invalid_count}.yaml`

**Registration:** single `add_test(NAME sonitude_unit_tests COMMAND sonitude_unit_tests)` — no Catch2/GTest; local `Require()` + exceptions.

**Helpers:** no shared signal-generator library. Synthetic sine lives in `calibration_capture.cpp` (500+100·ch Hz). Plan says M4 WAV harness / M5 mock trajectories go under `tests/integration/` (README only).

---

## 7. Conventions and layout

**Namespaces:** `sonitude::{app,audio,audio::alsa,dsp,rt,spatial,vad}`

**Errors:** `std::runtime_error` everywhere (config parse/validate, WAV, ALSA open, calibration). No `std::expected` / error codes. Tools/main catch `std::exception` and return nonzero.

**Layout today:**

- `src/app/` — YAML + logging (not RT)
- `src/audio/` — PCM/WAV; `alsa/` gated by `SONITUDE_WITH_ALSA`
- `src/dsp/` — calibration + resampler/ASRC
- `src/rt/` — lock-free + scheduling + counters
- `src/spatial/`, `src/vad/` — header-only stubs
- **No** `src/beamformer/`, `src/control/`, `src/odas/`, `src/calibration/` (calibration split across `app/` + `dsp/`)

Architecture doc target: fanout → RT audio + control; control publishes atomic steering snapshots — **not coded yet**.

---

## 8. ODAS integration present today

**Present:**

- `OdasConfig` parsed (`enabled`, `use_mock_provider`, `endpoint`)
- Docs: ODAS control-only, mock→adapter, failsafe ramp (`architecture.md`, plan, `device_setup.md` startup line)
- `SourceObservation` type suitable for DOA events
- Integration README: "ODAS mock trajectory and state-machine transition checks" planned

**Absent:** no `IDoaProvider`, mock provider, IPC client, source association, or steering publication.

Plan assumption: *"No installed ODAS instance or IPC contract is known. Keep `IDoaProvider` mockable…"*

`wav_replay.cpp` explicitly: *"Milestone 4 will implement offline six-channel replay/render."*

---

## 9. CMake targets — adding sources/tests

**Libraries**

- `sonitude_core` — always: app/dsp/rt/wav sources; PUBLIC include `src/`
- `sonitude_audio_alsa` — Linux only when `SONITUDE_WITH_ALSA=ON`

**Executables**

- `sonitude_realtime`
- `sonitude_device_probe`
- `sonitude_capture_check` / `sonitude_playback_check` / `sonitude_loopback_diag` (ALSA only)
- `sonitude_calibration_capture` / `sonitude_calibration_estimate`
- `sonitude_latency_marker` / `sonitude_wav_replay` (unlinked stubs)
- `sonitude_unit_tests` (+ CTest name of same)

**To add M4–M6 code:** append `.cpp` files to `sonitude_core` (or a new library linked to it); put headers under `src/<module>/`; add `tests/unit/<name>_tests.cpp` to `sonitude_unit_tests` sources and call its `Run…()` from `config_tests.cpp`'s `main`. Options: `SONITUDE_BUILD_TESTS`, `SONITUDE_FETCH_DEPS`, `SONITUDE_WITH_ALSA`, `SONITUDE_WITH_LIBSAMPLERATE` (define currently hard-disabled to 0).

---

## 10. Planning takeaways for M4–M6

1. **M4** can hang off existing `GeometryConfig`, `SteeringConfig`, `BeamformerSteering`, WAV I/O, and `sonitude_wav_replay` stub; implement fractional delay-and-sum + `steering_ramp_ms` smoothing; fill `delay_samples` gap if calibration delay stays in M3.
2. **M5** config/mock flags and `SourceObservation` are ready; need `IDoaProvider` + atomic snapshot handoff (architecture requires it; no helper yet).
3. **M6** timings/zones/`IVad` are ready; need state machine + wrap-safe zone logic (rear zone crosses ±180) + hysteresis tests under `tests/unit` or `tests/integration`.
