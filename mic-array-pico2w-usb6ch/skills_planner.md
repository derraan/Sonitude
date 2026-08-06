# CDE3301 PROJECT — GENERAL SKILLS PLANNER

## `skills_planner.md`

**Scope:** Whole CDE3301 project workspace
**Purpose:** Generalizable coding principles and diagnostic skills for Cursor
to instantiate specific skill responses when problems arise.
**Not hardware-specific. Not project-specific. Always active.**

---

## FOUNDATIONAL PRINCIPLE — THE THREE-LAYER CONTRACT

Every sub-project, module, and component in this workspace obeys a
three-layer architecture. The layers change names per sub-project
(embedded firmware, PC backend, UI frontend) but the contract is identical:

```
┌──────────────────────────────────────────────────┐
│  LAYER 3 — APPLICATION / BEHAVIOUR LAYER         │
│  What the system does. User-facing logic.         │
│  No hardware constants. No transport details.     │
│  Calls Layer 2 interfaces only.                   │
├──────────────────────────────────────────────────┤
│  LAYER 2 — PLATFORM / INTEGRATION LAYER          │
│  How the system connects components.              │
│  Drivers, middleware, protocol handlers,          │
│  library wrappers. Hardware-aware but not         │
│  hardware-locked. Uses Layer 1 constants.         │
├──────────────────────────────────────────────────┤
│  LAYER 1 — HARDWARE / ENVIRONMENT LAYER          │
│  What the system runs on.                         │
│  Board targets, SDK paths, system clock,          │
│  OS-level config, environment variables.          │
│  Ground truth for all hardware constants.         │
└──────────────────────────────────────────────────┘
```

**The Golden Rule:**

> Constants flow DOWN (Layer 1 → 2 → 3 via abstraction).
> Calls flow UP (Layer 3 → 2 → 1 via interfaces).
> Nothing hardcoded in Layer 3. Nothing application-specific in Layer 1.

---

## HOW CURSOR USES THIS FILE

1. **On project start** — read this file to understand active principles
2. **When a problem occurs** — match the symptom to a Skill below
3. **When a Skill is invoked** — generate a specific, context-aware
   sub-plan for the current sub-project using the Skill template
4. **When a new pattern is discovered** — append it to the
   `Discovered Patterns` section at the bottom of this file
5. **Layer divergence always takes priority** — if a Skill is being
   applied but layer divergence is detected mid-execution, switch to
   `SKILL A` immediately before resuming

---

## SKILL A — LAYER INTEGRITY & RESTORATION

**Merged from:** Layer Restoration, Hardware Migration, Constants Audit

### Trigger Conditions (any one is sufficient)

- A hardware constant (frequency, pin number, board name, OS path)
  appears in Layer 3
- A user-facing behaviour is coupled to a transport detail
  (e.g., baud rate in application logic)
- A build target change breaks application-layer code
- Porting to a new hardware/OS causes cascading changes across all layers
- Any `grep` for a platform literal returns matches in Layer 3 files

### Diagnostic — Locate the Divergence

**On Windows (PowerShell) — use ripgrep if available, otherwise:**

```powershell
# For embedded: search for clock frequencies, board names
Select-String -Recurse -Pattern "125000000|48000|pico|RP2040|RP2350" `
  -Include "*.c","*.h" -Path <layer3_directory>

# For PC backend: search for hardcoded ports, paths, device names
Select-String -Recurse -Pattern "COM[0-9]|localhost|127\.0\.0\.1|44100|16000" `
  -Include "*.py" -Path <layer3_directory>
```

**On Linux/macOS (bash):**

```bash
grep -rn "125000000\|48000\|pico\|RP2040\|RP2350" \
  <layer3_directory>/**/*.{c,h,py,ts,js}
```

### Restoration Steps

1. **Locate** — identify every violation using the diagnostic above
2. **Classify** — determine which layer the constant belongs to (usually L1)
3. **Extract** — move the constant to the correct layer:
   - Embedded: `CMakeLists.txt`, board header, or SDK call
     (`clock_get_hz(clk_sys)`)
   - PC: `.env` file, `config.py`, or environment variable
   - Frontend: `constants.ts` / `config.json`
4. **Abstract** — replace the literal in Layer 2/3 with a named reference
   or function call that fetches from Layer 1
5. **Verify** — re-run the diagnostic; expect zero matches
6. **Commit** with message prefix: `fix(layerN): extract [constant] to layer [target]`

### Layer Reference Table — adapt per sub-project

| Sub-project         | Layer 1                          | Layer 2                          | Layer 3                       |
|:------------------- |:-------------------------------- |:-------------------------------- |:----------------------------- |
| Pico firmware       | CMakeLists, board header         | PIO drivers, TinyUSB             | Application main.c            |
| PC audio pipeline   | `.env`, `config.py`              | Audio I/O handler, model loader  | Inference logic, UI callbacks |
| Sound Bubble host   | OS audio config, CUDA/CPU target | Chunking engine, device selector | Spatial filter application    |
| Future sub-projects | Environment / build config       | Transport / interface layer      | Behaviour / output layer      |

---

## SKILL B — BUILD & ENVIRONMENT INTEGRITY

**Merged from:** Clean Build Protocol, Flash Verification, Toolchain Setup

### Trigger Conditions

- Build succeeds but binary behaves incorrectly after flash/deploy
- Incremental build produces different output than clean build
- Compiler target or toolchain version changed
- New dependency added without cache invalidation
- `cmake` output does not confirm expected platform/target

### Diagnostic — Confirm Environment State

```powershell
# Windows PowerShell — Embedded (Pico SDK)
cmake --version                          # must be >= 3.13
arm-none-eabi-gcc --version              # confirm toolchain present
echo $env:PICO_SDK_PATH                  # must point to SDK v2.1.0+
Get-Command arm-none-eabi-gcc            # must resolve — if not, add to PATH

# Required env vars for every PowerShell build session:
$env:PICO_SDK_PATH = "C:\Users\<user>\pico\pico-sdk-2.1.0"
$env:PATH = "C:\Users\<user>\.pico-sdk\toolchain\13_2_Rel1\bin;$env:PATH"
$env:PICO_TOOLCHAIN_PATH = "C:\Users\<user>\.pico-sdk\toolchain\13_2_Rel1"
```

```bash
# Linux/macOS — Embedded (Pico SDK)
cmake --version          # must be >= 3.13
arm-none-eabi-gcc --version
echo $PICO_SDK_PATH      # must point to SDK v2.1.0+
```

```bash
# PC (Python — any platform)
python --version
pip list | grep -E "sounddevice|numpy|torch|scipy"
```

**Confirm build target in CMake output (embedded):**

```
Pico Platform (PICO_PLATFORM) is 'rp2350-arm-s'.   ← correct for pico2_w
Defaulting compiler (PICO_COMPILER) to 'pico_arm_cortex_m33_gcc'
```

**Confirm target architecture in ELF:**

```bash
arm-none-eabi-readelf -A <output>.elf | grep "CPU_arch"
# Expected for RP2350: Tag_CPU_arch: v8-M.mainline  (= Cortex-M33)
# Expected for RP2040: Tag_CPU_arch: v6-M            (= Cortex-M0+)
```

### Resolution — Universal Clean Build

```powershell
# Windows PowerShell
Remove-Item -Recurse -Force build
New-Item -ItemType Directory build | Out-Null
Set-Location build
cmake -DCMAKE_BUILD_TYPE=Debug -G Ninja ..
cmake --build . -- -j4
```

```bash
# Linux/macOS
rm -rf build/
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . -- -j$(nproc)
```

Confirm artefact exists and is non-zero size:

```powershell
Get-ChildItem -Recurse build\ -Include *.uf2   # embedded (Windows)
```

```bash
ls -lh **/*.uf2   # embedded (Linux/macOS)
```

### Flash Verification Checklist (Embedded)

| Check                      | RP2040 Expected      | RP2350 Expected      |
|:-------------------------- |:-------------------- |:-------------------- |
| Drive label on BOOTSEL     | `RPI-RP2`            | `RP2350`             |
| Compiler flag in build log | `cortex-m0plus`      | `cortex-m33`         |
| ELF CPU arch tag           | `v6-M`               | `v8-M.mainline`      |
| CMake PICO_PLATFORM        | `rp2040`             | `rp2350-arm-s`       |
| Windows Device Manager     | USB Audio / COM port | USB Audio / COM port |

> **RP2350 spinlock build blocker:** if build fails with
> `#error no SW_SPIN_TRY_LOCK available for PICO_USE_SW_SPIN_LOCK on this platform`,
> treat this as a real environment/config mismatch (not transient). Confirm SDK
> version/toolchain pairing, inspect where `PICO_USE_SW_SPIN_LOCK` is introduced,
> and align project config to RP2350-supported locking paths before code changes.
> 
> **Important refinement:** do not pass sample-rate overrides through
> `-DCMAKE_C_FLAGS="..."` in Pico builds. This can clobber required SDK target
> flags for C compilation and surface as misleading spinlock/assembler failures.
> Prefer target/project cache vars (for example
> `-DUSB_AUDIO_SAMPLE_RATE_OVERRIDE=24000`) so RP2350 arch flags stay intact.
> 
> **"Unable to find definition of board 'pico2_w'":** SDK version is too old.
> Upgrade to SDK v2.1.0+. This is not a code problem.
> 
> **picotool auto-fetch on first cmake configure:** SDK 2.1.0 will clone
> picotool from git if not pre-installed. Adds ~2 min. Normal behaviour.

### Deployment Verification (PC)

- Confirm process starts without import errors
- Confirm audio device is enumerated and selectable
- Confirm expected channels / sample rate in device properties
- Run a 5-second capture and inspect output for continuity

---

## SKILL C — INTERFACE CONTRACT VERIFICATION

**Merged from:** USB Descriptor Integrity, Serial Protocol Integrity,
Audio Format Consistency

### Trigger Conditions

- Data arrives at Layer 3 but is malformed, offset, or silent
- Channel count, sample rate, or bit depth mismatch between producer and consumer
- USB device enumerates but audio/data is garbled
- Serial/WebSocket messages parse incorrectly
- PC-side model receives wrong tensor shape

### Core Principle — Interface Contracts

Every boundary between two components is a **contract**. Both sides must
agree on:

```
[ FORMAT ] — data type, bit depth, encoding
[ RATE ]   — sample rate, baud rate, chunk size, polling interval
[ WIDTH ]  — channel count, tensor dimensions, message field count
[ TIMING ] — latency budget, timeout, buffer size
```

### USB Audio Naming Convention (microphone devices)

In TinyUSB, audio flowing FROM device TO host (microphone → USB) is named
from the **device's perspective**:

- `CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX` — channel count for mic data sent to host
- `CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX` — channel count for playback received from host

A mic array uses `_TX`. Plans or docs that say `_RX` for a microphone device
are using the wrong variable name.

### Channel Count vs Physical Mic Count

The USB-reported channel count may be higher than the physical mic count.
This is intentional when:

- The I2S bus has multiple data lanes (e.g. 3 lanes × L+R = 6 active + 2 silent)
- The hardware layout requires padding to align to a power of 2

Before assuming a channel count mismatch is a bug, check:

1. Does the product string name explain the discrepancy (e.g. "8ch (6 active)")?
2. Are the extra channels explicitly set to `0` in the audio buffer filling code?
3. Is the DMA buffer indexing consistent with the reported channel count?

If all three are yes: this is a design decision, not a bug. Document it and
preserve it. Do not reduce the channel count without tracing all buffer index
arithmetic.

### Diagnostic — Find the Broken Contract

1. Identify the **interface boundary** where data goes wrong
   (not where the symptom appears — trace backwards)
2. Document what each side of the boundary **expects**
3. Document what each side **actually sends/receives**
4. The gap between expected and actual = the broken contract term

### Resolution Steps

1. **Pick one side as ground truth** (usually the producer / hardware side)
2. **Update the consumer** to match producer's actual output — or vice versa
   if the producer is wrong
3. **Document the agreed contract** as named constants in Layer 1:

```c
// Embedded example
#define AUDIO_SAMPLE_RATE_HZ   16000
#define AUDIO_CHANNEL_COUNT    8     // USB-reported; 6 physically active
#define AUDIO_BIT_DEPTH        32
```

```python
# PC example
AUDIO_SAMPLE_RATE_HZ = int(os.environ.get("AUDIO_SAMPLE_RATE", 16000))
AUDIO_CHANNEL_COUNT  = int(os.environ.get("AUDIO_CHANNELS", 8))
```

4. **Reference these constants** on both sides of the interface —
   never duplicate literal values across the boundary
5. **Regression test** — record raw interface data, verify against contract

---

## SKILL D — PORTING & MIGRATION PATTERN

**Trigger Conditions**

- Moving code from one hardware target to another
- Moving code from one OS/environment to another
- Upgrading a dependency that changes an interface
- Integrating an external repo into the project

### Pre-Migration Environment Check (run before any code changes)

Before cloning or copying anything, verify:

1. The target SDK/runtime version supports the target board/platform
2. The toolchain is available and produces the correct target architecture
3. Any third-party libraries (TinyUSB, etc.) have compatible APIs in the new SDK
4. The new SDK's bundled library versions are known (check `lib/tinyusb` git
   submodule hash if relevant)

Skipping this check is the leading cause of mid-migration environment
surprises that are not code problems but look like code problems.

### The Migration Sequence (always in this order)

```
STEP 1: ESTABLISH BASELINE
  → Clone / copy source as-is into new repo
  → Commit with tag: upstream-baseline
  → DO NOT modify anything yet

STEP 2: ISOLATE LAYER 1 CHANGES ONLY
  → Change only the hardware/environment target
  → Build and verify it compiles for new target
  → Commit: fix(layer1): migrate to [new target]

STEP 3: ISOLATE LAYER 2 CHANGES ONLY
  → Fix platform-layer incompatibilities (clock dividers,
    driver APIs, transport config)
  → Build and verify basic hardware behaviour
  → Commit: fix(layer2): resolve [platform] incompatibilities

STEP 4: PORT APPLICATION CODE
  → Diff original application layer against new Layer 2 interface
  → Apply additive changes only — never rewrite from scratch
  → Commit: port(layer3): migrate application logic

STEP 5: VERIFY INTERFACE CONTRACTS (invoke SKILL C)
  → Confirm all interface contracts are intact end-to-end
```

> **Rule:** If any step requires touching two layers simultaneously,
> STOP. Invoke SKILL A first. Restore layers, then resume the sequence.

> **Rule:** Original examples from a cloned library repo that use an
> incompatible API version should be DISABLED (commented out of CMakeLists),
> not fixed. Fixing them is out of scope unless they are needed for the target
> application. Document the reason with a CURSOR NOTE comment.

### Repo Naming Convention for Migrations

```
<function>-<hardware-target>-<transport>

# Examples:
mic-array-pico2w-usb6ch
soundbubble-host-win-python
hrtf-pipeline-pc-realtime
```

---

## SKILL E — DEBUG TRIAGE PROTOCOL

**Trigger:** Any unexpected behaviour where the root cause is not
immediately obvious.

### Triage Order (follow strictly — do not skip levels)

```
Level 1 — Is the environment correct?
  → Invoke SKILL B diagnostic. Confirm toolchain, SDK, dependencies.
  → If environment is wrong, fix it before any code changes.

Level 2 — Is the interface contract intact?
  → Invoke SKILL C diagnostic. Confirm format/rate/width/timing.
  → If contract is broken, fix it before investigating logic.

Level 3 — Is the layer architecture intact?
  → Invoke SKILL A audit. Confirm no layer violations.
  → If violations exist, restore layers before investigating logic.

Level 4 — Is the logic correct?
  → Only reach here after Levels 1–3 are confirmed clean.
  → Add targeted logging at layer boundaries to isolate.
  → Fix the smallest possible change. Re-verify all layers after.
```

> **Anti-pattern to avoid:** Jumping to Level 4 first.
> Most bugs in embedded + PC cross-platform projects are Level 1–3.

---

## SKILL INVOCATION QUICK REFERENCE

| Symptom                                              | Primary Skill | Secondary                                                                                                |
|:---------------------------------------------------- |:------------- |:-------------------------------------------------------------------------------------------------------- |
| Hardcoded constant in wrong file                     | **SKILL A**   | —                                                                                                        |
| Porting to new hardware/OS                           | **SKILL D**   | SKILL A after each step                                                                                  |
| Build succeeds but wrong behaviour                   | **SKILL B**   | SKILL C                                                                                                  |
| Audio/data garbled at interface                      | **SKILL C**   | SKILL A                                                                                                  |
| Silent channel or missing data                       | **SKILL C**   | SKILL B                                                                                                  |
| New repo setup for sub-project                       | **SKILL D**   | SKILL B                                                                                                  |
| Cascading changes across all layers                  | **SKILL A**   | SKILL D                                                                                                  |
| Unexplained bug, no obvious cause                    | **SKILL E**   | All skills in order                                                                                      |
| Metallic / ring-modulator distortion on all channels | **SKILL C**   | FW-CRIT-7 (no zero-fill), FW-CRIT-14 (ring buffer sizing), FW-CRIT-15 (read request matches frame count) |
| cmake: "Unable to find definition of board"          | **SKILL B**   | Check SDK version (must be v2.1.0+ for pico2_w)                                                          |
| TinyUSB compile error on `tusb_init` args            | **SKILL B**   | TinyUSB 0.16 API requires zero-arg `tusb_init()`                                                         |
| Drive appears as `RPI-RP2` not `RP2350`              | **SKILL B**   | Check `PICO_BOARD` value in CMakeLists.txt                                                               |

---

## CRITICAL FIRMWARE SAFETY GUARDRAILS (MANDATORY)

These guardrails are **non-optional** for any firmware touching USB audio,
DMA/PIO, clocks, flashing, or host audio routing. Violate none.

| ID         | Guardrail Rule                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Failure if Violated                                                                                                                                                                                                                                                                                                                                                       |
|:---------- |:--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |:------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| FW-CRIT-1  | Keep USB descriptor/control contract strict and deterministic. Descriptor clock attributes/controls must exactly match firmware behavior (fixed RO or variable RW).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | Host control-path instability, invalid-device open errors, DAW freeze/replug loops.                                                                                                                                                                                                                                                                                       |
| FW-CRIT-2  | Never perform blocking waits in USB ISR/callback paths (`tx_pre`, `tx_post`, control callbacks). Use non-blocking ring/FIFO access and zero-fill fallback.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | USB service starvation, endpoint underruns, host audio engine stalls that can disrupt other system audio devices.                                                                                                                                                                                                                                                         |
| FW-CRIT-3  | Packet sizing and frame math must derive from one active source of truth (current stream rate/state), not mixed compile-time/runtime literals.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Isochronous packet mismatch, glitch bursts, random dropouts, application-level freezes.                                                                                                                                                                                                                                                                                   |
| FW-CRIT-4  | Apply sample-rate or transport mode changes only at safe boundaries (stream stopped / alt 0 / idle state), then re-prime state before restart.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Mid-stream state corruption, dead streams, hard-to-recover host errors requiring unplug/replug.                                                                                                                                                                                                                                                                           |
| FW-CRIT-5  | Preserve defensive telemetry in production builds (DMA IRQ count, overrun count/bytes, stream-alt transitions, init error code). Do not remove observability during refactors.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Silent regressions and delayed diagnosis; unsafe releases that appear healthy until field failure.                                                                                                                                                                                                                                                                        |
| FW-CRIT-6  | Maintain host identity stability across normal firmware iterations (VID/PID/bcd usage policy must be deliberate). Only force re-enumeration when descriptor shape actually changed.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | Endpoint/cache churn, default-device confusion, playback routing failures on host OS.                                                                                                                                                                                                                                                                                     |
| FW-CRIT-7  | On data starvation, output **continuity-held audio** (last decoded sample per channel, decaying exponentially ~6.25%/ms via `>> 4` shift). On recovery from total starvation, apply a 32-frame soft re-entry crossfade (linear blend decayed-held to live audio over ~0.7 ms). **Never zero-fill or `memset` partial frames mid-stream.** Zero-filling creates hard silence edges that produce impulse transients at every starvation boundary; at any periodic starvation rate these transients produce ring-modulator AM distortion audible across the full frequency range.                                                                                                                                                                                                                            | Hard silence edges produce high-energy vertical spectrogram spikes at every starvation event, which at periodic rates become audible ring-mod distortion. `memset` fill on short reads is the confirmed primary source of ring-mod distortion in this pipeline — it must be replaced with sample-hold on every code path.                                                 |
| FW-CRIT-8  | Any change to clocking, USB descriptors, callback timing, or DMA buffer contract requires full stage-gate validation before merge/release.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | "Builds/flashes but unsafe" firmware reaching users; recurring crashes/freezes and trust loss.                                                                                                                                                                                                                                                                            |
| FW-CRIT-9  | For selectable-rate firmware, only apply sample-rate changes at safe boundaries: apply immediately in idle (`alt=0`), defer while streaming (`alt=1`), and commit on stop/restart.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | Mid-stream reconfiguration races, endpoint glitches, host stream open/record failures.                                                                                                                                                                                                                                                                                    |
| FW-CRIT-10 | In selectable-rate mode, advertise only validated discrete rates and reject unsupported host requests. Keep `GET_CUR`, `GET_RANGE`, and `SET_CUR` mutually consistent.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | Host picks invalid format, clock mismatch, silent recordings or repeated open failures.                                                                                                                                                                                                                                                                                   |
| FW-CRIT-11 | In selectable-rate firmware, **both** `USB_FRAME_SAMPLES_MAX` (internal DMA/USB buffer ceiling) **and** `CFG_TUD_AUDIO_EP_SZ_IN` (endpoint max-packet in the descriptor) must be sized for the **highest supported runtime rate** (48 kHz), not the compile-time default.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | `USB_FRAME_SAMPLES_MAX` undersized → silent buffer overflow when host selects higher rate. `CFG_TUD_AUDIO_EP_SZ_IN` undersized → host rejects device open with `paInvalidDevice` / error `-9996`. Both fail silently at compile time.                                                                                                                                     |
| FW-CRIT-12 | Bump `USB_PID`/`bcdDevice` whenever the **descriptor shape** changes (clock type, clock control bits, number of sub-ranges, endpoint max-packet size). Windows caches descriptors per PID; if the shape changes without a PID bump, the host may use a stale cached format that does not match new firmware.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | Stale Windows cache causes persistent `-9996` / invalid device errors that survive reflash until PID changes force fresh OS enumeration.                                                                                                                                                                                                                                  |
| FW-CRIT-13 | **USB input device enumeration must never break host system audio output (speakers/headphones).** If plugging in the USB mic device kills or wedges the Windows audio engine (speakers go silent, AudioSrv/AudioEndpointBuilder wedge, Nahimic/IntelAudio locks up), this is a **hard release blocker** — not a "separate issue to fix later". Root causes: USB isochronous descriptor too wide for the host controller bandwidth budget, missing `NON_MAX_PACKETS_OK` flag, or endpoint size change that triggers Windows audio session rebuild on plug-in. Must be confirmed clean at the **Audio Coexistence Gate** before any release.                                                                                                                                                                | Host speakers/headphones stop working on Pico plug-in. Windows audio service wedges requiring full reboot to recover. Other apps lose audio output. User must reboot to restore system audio.                                                                                                                                                                             |
| FW-CRIT-14 | **Ring-buffer sizing must be anchored to worst-case runtime rate and include enough jitter headroom.** In selectable-rate firmware, define ring length from `AUDIO_SAMPLE_RATE_MAX` (48 kHz in this project), channel count, and sample width — not from a fixed DMA constant alone. `SIZEOF_DMA_BUFFER_IN_BYTES` is hardware-fixed (1536 bytes) and does not scale with runtime rate selection. Required formula shape: `ring_len = bytes_per_ms_at_max_rate * headroom_ms`; use at least `32 ms` headroom in this project (implemented as `* 32u` multiplier). Also see FW-CRIT-15 for the companion read-request size rule. Also preserve the non-negotiable floor `usable_ring_bytes >= 2 * SIZEOF_HALF_DMA_BUFFER_IN_BYTES + 1`, because the byte ring sacrifices one slot for full/empty detection. | Wrong anchor or insufficient multiplier causes deterministic DMA push loss and periodic zero-filled USB frames. Audible symptom: persistent ring-modulator/metallic AM artifact (≈ 919 Hz at 44.1 kHz case). Visual symptom: regular high-energy spectrogram spikes at drop boundaries. Silent at compile time; only visible via overrun telemetry and waveform analysis. |
| FW-CRIT-15 | **Ring buffer read request must equal the actual USB frame count.** Call `microphone_array_i2s_read_stream_nonblocking` with `request_bytes = usb_tx_frame_samples * (4 * I2S_RX_FRAME_SIZE_IN_BYTES)`, not `sizeof(buffer)`. At 44.1 kHz: DMA fills at 1411 B/ms; `sizeof(buffer) = 1568 B` drains the ring at 1568 B/ms, depleting a 28 KB pre-fill buffer in ~180 ms and silently restarting starvation. Requesting exactly 44-45 frames (1408-1440 B) matches DMA fill rate and keeps the ring stable indefinitely. Corollary: USB drain rate must never exceed DMA fill rate at any supported sample rate.                                                                                                                                                                                           | Ring drains ~157 B/ms faster than DMA fills; pre-fill buffer depleted in ~180 ms; starvation restarts and ring-mod returns. Symptom identical to FW-CRIT-14 because the root cause is still starvation — only the driver differs. Confirmed hardware root cause for the final ring-mod after all other fixes were applied.                                                |

### Mandatory Stage-Gate Before Release

1. **Build Gate** — clean configure/build passes with expected target/SDK.
2. **Enumeration Gate** — device enumerates consistently across replug cycles.
3. **Audio Coexistence Gate** — recording from USB mic does not break playback/output devices. **Plug-in test mandatory:** plug Pico in while speakers are actively playing audio and confirm speakers remain working. If speakers die, wedge, or require a reboot to restore, this gate fails. (FW-CRIT-13)
4. **Glitch Recovery Gate** — injected/dropout conditions recover without DAW freeze or system audio lockup.
5. **Contract Gate** — descriptor/control semantics match intended fixed-rate or selectable-rate policy exactly.
6. **Telemetry Gate** — counters/logs confirm bounded overruns and healthy callback cadence.
7. **Ring Balance Gate** — at 44.1 kHz under sustained capture, confirm DMA overrun count is bounded (not monotonically growing) and `usb_tx_frame_samples` equals the target frame count (44/45) on every callback. If overruns grow without bound, the ring is draining faster than DMA fills — check read request size (FW-CRIT-15).

> **Hard stop policy:** if any gate fails, do not ship, do not tag release, do not
> advise users to rely on that firmware.

---

## DISCOVERED PATTERNS

> Cursor: Append new patterns here as they are discovered during
> project execution. Include: trigger, sub-project context, resolution.

| Date       | Sub-project                            | Pattern Discovered                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  | Skill Updated                  |
|:---------- |:-------------------------------------- |:----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |:------------------------------ |
| 2026-03-31 | mic-array-pico2w-usb6ch                | RP2350 Pico SDK builds may emit UF2 `ABSOLUTE_FAMILY_ID` (`0xe48bff57`) for non-partitioned firmware; this is valid on RP2350 BOOTSEL and must not be mistaken for RP2040.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          | SKILL B                        |
| 2026-03-31 | mic-array-pico2w-usb6ch                | USB Full Speed `44.1 kHz` audio needs variable `44/45`-sample 1 ms packets; INMP441 I2S capture stays 32-bit internally; UAC2 Type-I PCM in this project uses **4 bytes per channel** on USB (aligned with RP2040 reference), not 16-bit-only unless explicitly changed.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | SKILL C                        |
| 2026-03-31 | mic-array-pico2w-usb6ch                | When `pico_stdio_usb` baseline enumerates but manual TinyUSB CDC does not, compare against TinyUSB device examples and prefer `tud_init(BOARD_TUD_RHPORT)` over generic `tusb_init()` for the Pico device path.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | SKILL B                        |
| 2026-03-31 | mic-array-pico2w-usb6ch                | Windows can cache prior USB audio descriptors by VID/PID and keep showing an old product name or channel layout after reflashing. During staged USB debugging, assign distinct `PID`/`bcdDevice` values per debug stage to force fresh enumeration.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | SKILL C                        |
| 2026-03-31 | mic-array-pico2w-usb6ch                | For USB audio bring-up on Pico, use a strict staged ladder: `Stage 0` minimal `pico_stdio_usb`, `Stage 3` manual TinyUSB CDC, `Stage 4` minimal 2-channel UAC2, then `Stage 5` full audio path. This isolates transport, class, and descriptor failures cleanly.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | SKILL E                        |
| 2026-03-31 | mic-array-pico2w-usb6ch                | UAC2 Type-I PCM: align `CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX`, FIFO buffers, and `i2s_to_usb_*` with the **RP2040** reference (4 bytes per channel, `uint32_t` buffers, pre-load/post-load order). A mismatch (e.g. 16-bit app vs 32-bit descriptor) or skipping `refresh_i2s_connections()` on Stage 4 breaks streaming. See `docs/UAC2_DESCRIPTOR_NOTES.md`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | SKILL C                        |
| 2026-04-01 | mic-array-pico2w-usb6ch + Sound_Bubble | The current Pico USB firmware enumerates to Windows as **8-channel UAC2 at 16 kHz, 32-bit**, but only the first 6 channels carry live INMP441 mic data; channels 7-8 are intentionally zero-filled to preserve the original DFRobot lane layout. Host-side consumers must treat this as an `8 exposed / 6 active` contract rather than true 8-mic capture.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          | SKILL C                        |
| 2026-04-01 | mic-array-pico2w-usb6ch + Sound_Bubble | Audacity successfully recording an endpoint does **not** prove PortAudio/`sounddevice` can open the same endpoint with the same width/rate/channel count. On Windows, always validate the PC-side contract separately with `query_devices()` and explicit `check_input_settings()` / `check_output_settings()` before assuming the host path is correct.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | SKILL B                        |
| 2026-04-01 | mic-array-pico2w-usb6ch + Sound_Bubble | A realtime host pipeline should not assume one duplex device sample rate. For this project, USB mic capture can require `16 kHz` while playback devices often require `48 kHz`; use separate input/output streams plus simple host resampling between capture, model rate, and playback rate.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | SKILL C                        |
| 2026-04-01 | mic-array-pico2w-usb6ch + Sound_Bubble | When one exposed USB channel is noisy or pinned near `0 dB`, the host integration layer should support explicit channel muting and channel remapping before NN inference. Useful patterns include direct maps like `1,1,2,2,3,3` and cyclic expansion of shorter lists rather than blindly duplicating the final selected channel.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  | SKILL E                        |
| 2026-04-01 | mic-array-pico2w-usb6ch + Sound_Bubble | Realtime fidelity depends on matching chunk timing to model hop timing. If host `input_block` covers multiple model hops, the inference loop must process **every hop sequentially with persistent state** and concatenate outputs; collapsing a long block into one hop causes choppy/fragmented playback.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | SKILL C                        |
| 2026-04-01 | mic-array-pico2w-usb6ch + Sound_Bubble | Throughput guardrail: choose `block_multiplier` such that total per-block inference time stays below block wall-clock duration (`input_block / input_sr`). In this project, `block_multiplier=9` at `44.1k -> 24k` creates 9 model steps per callback and can push inference to ~150 ms for a ~72 ms block, causing queue drops, underruns, and `primed=False`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | SKILL B                        |
| 2026-04-01 | mic-array-pico2w-usb6ch + Sound_Bubble | For NN debugging, log levels at three boundaries: raw USB channels (`usb`), exact tensor fed to model (`mdl_in` post-resample/pad), and model output (`model`). This separates model-domain failures from output-path failures and prevents misdiagnosing silent output as device I/O issues.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | SKILL E                        |
| 2026-04-02 | Sound_Bubble                           | Before starting training from config files, verify the full contract `config import path -> module exists -> returned batch keys match trainer expectations`. In this project, adding missing dataset module wiring and `targets['num_noises']` compatibility avoided late runtime failures.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | SKILL C                        |
| 2026-04-02 | Sound_Bubble                           | For model-variant swaps (`dis_embd3` vs `optim`), treat model class, dataset output schema, and realtime preset selection as one interface migration. If any one of the three remains on the old contract, realtime bring-up can appear \"slow/broken\" even when inference kernels are healthy.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | SKILL D                        |
| 2026-04-02 | Sound_Bubble + Cursor                  | In IDE-integrated terminals, treat shell bootstrap as a Layer-1 environment contract. If `conda` is not on PATH in the integrated shell, register an explicit terminal profile (`conda-hook.ps1` + `conda activate <env>`) instead of assuming external Anaconda Prompt behavior carries over.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      | SKILL B                        |
| 2026-04-02 | Sound_Bubble                           | For distance-thresholded training data, enforce `dataset split distance == dis_threshold` per run (for example `syn_1m` with `1.0`, `syn_1_5m` with `1.5`, `syn_2m` with `2.0`). Mixing splits under one global threshold silently corrupts supervision targets.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | SKILL C                        |
| 2026-04-02 | Sound_Bubble                           | Realtime preset design should prefer deterministic local run-dir loading (`runs/.../config.json` + checkpoints) with explicit fallback configs. This reduces launch ambiguity and prevents accidental random-weight runs when `--checkpoint-path` is omitted.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | SKILL E                        |
| 2026-04-04 | Sound_Bubble + mic-array-pico2w-usb6ch | For model export, benchmark, and live inference parity, generate and consume one runtime contract JSON (`*.runtime.json`) as Layer-1 truth (`model_sr`, `num_ch`, `chunk`, `pad`, `frame_len`, channel map). Do not duplicate these as literals across scripts.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | SKILL C                        |
| 2026-04-04 | Sound_Bubble                           | Input-level dead-mic detection should use a short rolling RMS window for silence thresholds (for example `-50 dBFS`) rather than per-frame decisions, while `-1..0 dBFS` digital-fault gating remains instantaneous. This avoids false dead-mic flags during brief speech pauses.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | SKILL C                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | DMA overrun handling must be treated as an interface reliability contract, not just telemetry: keep overrun counters, expose them in CDC diagnostics, and gate higher-rate bring-up on zero-growth or bounded-growth criteria under sustained capture.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | SKILL E                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | INMP441 + Pico firmware can run native `24 kHz` capture if clocking and descriptors are explicitly configured (`AUDIO_SAMPLE_RATE == 24000`, matching PIO divider constants, TinyUSB sample-rate override). This should be preferred when host model input is fixed at `24 kHz`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | SKILL D                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | Scope-based clock checks must be sample-rate aware: WS target is the configured sample rate and SCK target is `sample_rate * 64` for 32-bit stereo slots per lane aggregation in this design. Do not hardcode `44.1 kHz / 2.8224 MHz` expectations when running 24 kHz or 48 kHz builds.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | SKILL C                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | For signal integrity on I2S clock outputs, explicitly set SCK/WS GPIO drive strength to `12mA` in firmware during GPIO setup and verify on hardware bring-up checks.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | SKILL A                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | RP2350 builds that fail with `SW_SPIN_TRY_LOCK` should no longer be classified as non-deterministic transient failures; treat as a hard stage-gate blocker until lock configuration is resolved for the selected SDK/board/toolchain tuple.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | SKILL B                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | In Pico SDK 2.1.0 TinyUSB integration, seeing `CFG_TUSB_MCU=OPT_MCU_RP2040` in compile lines can be expected for RP2 USB backend reuse and is not alone proof of wrong board targeting. Confirm board/platform truth from CMake (`PICO_BOARD`, `PICO_PLATFORM`) before diagnosing target mismatch.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  | SKILL B                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | UAC2 sample rate is now runtime-selectable (16k/24k/44.1k/48k) via clock control when descriptors advertise variable clock + RW frequency control and firmware handles `AUDIO_CS_CTRL_SAM_FREQ` GET/SET. Build-time sample-rate override should be treated as initial default, not a hard lock.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | SKILL C                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | Build configuration contract: pass audio-rate selection through dedicated CMake cache vars (`USB_AUDIO_SAMPLE_RATE_OVERRIDE`) instead of global `CMAKE_C_FLAGS` injection to avoid accidental loss of RP2350 C compile flags and false architecture/spinlock failures.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | SKILL B                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | Runtime-selectable UAC2 sample rates are valid only with strict control contract parity: clock source descriptor must be variable+RW, supported rates must be explicit discrete range entries, and `SET_CUR` must use idle/apply + streaming/defer semantics. This avoids `-9996 Invalid device` open failures and stream instability.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | SKILL C                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | Firmware safety policy is now explicit: no blocking USB callback paths, no mid-stream contract mutation, deterministic silence on starvation, and mandatory coexistence/glitch-recovery stage-gates before release to prevent host-audio lockups.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | SKILL E                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Audit finding:** In a selectable-rate UAC2 build, `USB_FRAME_SAMPLES_MAX` and `CFG_TUD_AUDIO_EP_SZ_IN` must both be sized for the **maximum runtime rate** (48 kHz), not the compile-time default. Sizing either for the default (e.g. 44.1 kHz) causes silent buffer overflow at runtime or host open rejection (`-9996`) when the host selects a higher rate.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | SKILL C, FW-CRIT-11            |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Audit finding:** Changing descriptor shape (clock type, control bits, sub-range count, endpoint max packet) without bumping `USB_PID`/`bcdDevice` leaves Windows serving a stale cached descriptor for the new firmware. The resulting format mismatch causes persistent `-9996` errors that survive reflash. Always bump PID/bcd on any structural descriptor change.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | SKILL C, FW-CRIT-12            |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Field failure:** Plugging in the Pico USB mic device wedged the Windows audio engine (speakers stopped working, AudioSrv/Nahimic locked, required reboot to recover). Root cause: host audio session rebuild triggered by USB device enumeration can lock exclusive access on the default output endpoint. This is a **hard release blocker**, not a cosmetic issue. Must be validated at the Audio Coexistence Gate before release.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | SKILL E, FW-CRIT-13            |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Root cause (v1):** Passing `SIZEOF_DMA_BUFFER_IN_BYTES` (1536) as ring buffer length to `create_microphone_array_i2s` leaves only 1535 usable bytes — exactly one DMA half-buffer (768) push worth of space. The second push is always dropped by `empty_dma()`. At 44.1 kHz this silently discards every alternate 24-frame block, creating ≈ 919 Hz amplitude modulation heard as ring-modulator/metallic distortion.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           | SKILL C, FW-CRIT-14            |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Root cause (v2): compounding ring-buffer sizing error.** The first mitigation (`*8`) reduced but did not eliminate distortion because it was anchored to `SIZEOF_DMA_BUFFER_IN_BYTES` (fixed hardware constant) instead of maximum runtime demand. Correct policy mirrors USB endpoint sizing: anchor to `AUDIO_SAMPLE_RATE_MAX` and use larger jitter headroom. Implemented formula: `SIZEOF_I2S_RING_BUFFER_IN_BYTES = (((AUDIO_SAMPLE_RATE_MAX * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX * sizeof(int32_t)) / 1000u) * 16u)`. This aligns ring sizing with worst-case selectable-rate behavior (16k/24k/44.1k/48k).                                                                                                                                                                                                                                                                                                                                                                 | SKILL C, FW-CRIT-14            |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Windows UAC2 default rate = first `sampleFreqRng.subrange[0]` entry.** Windows picks the first sub-range in the firmware's `GET_RANGE` response as its shared-mode default format at first enumeration. If `subrange[0]` is 16 kHz, Windows defaults to 16 kHz/16-bit despite other rates being available. To default to 44100 Hz, reorder `sampleFreqRng` so `subrange[0].bMin = 44100`. Changing the Windows shared-mode format via Sound Settings while an audio client is live causes a hang (UAC2 `SET_CUR` stall). Safe workaround: uninstall device in Device Manager, replug, then set format before any audio app opens the device.                                                                                                                                                                                                                                                                                                                                      | SKILL C                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Audacity -9996 on selectable-rate UAC2:** If Audacity opens the device at a rate different from the Windows shared-mode default (e.g. project rate 44100 but Windows default is 16000), it returns `-9996 Invalid device`. Fix without reflashing: set Audacity Host to **Windows WASAPI**, match the Audacity project rate to the Windows default (16000 in this case), and confirm audio opens cleanly. This validates the DMA ring buffer fix independently of any rate-default issue.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | SKILL C                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **`picotool.exe` file lock on `Remove-Item` during `rebuild_and_flash.ps1 -Rebuild`:** Windows holds `picotool.exe` locked in the build `_deps` folder if it was previously run or cached by another process. `Stop-Process` may not release the lock if the process is wedged in kernel/driver code. Workarounds in priority order: (1) `taskkill /F /IM picotool.exe /T` from CMD, (2) Device Manager → uninstall Pico, replug, retry, (3) build to an alternate directory (`-B build_fresh`) to avoid the locked path entirely.                                                                                                                                                                                                                                                                                                                                                                                                                                                  | SKILL B                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Ring-modulator artifact root cause (confirmed in hardware): zero-fill on short I2S reads.** When `usb_audio_post_load_fill` received fewer bytes than the full 1 ms USB frame, the output buffer tail was zero-filled (`memset`). Each zero-edge produces a high-energy impulse visible as a vertical spectrogram spike; periodic starvation turns these into amplitude modulation heard as ring-modulator distortion. Fix: replace zero-fill with `last_good_sample[ch]` continuity hold. Add exponential DC-blocking decay (`>> 4` per callback ≈ 16 ms) to prevent sustained DC offset during long starvation. Confirmed: vertical spikes eliminated in spectrogram, ring-mod character gone.                                                                                                                                                                                                                                                                                  | SKILL C, FW-CRIT-7             |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **USB FS isoch payload hard limit at 44.1 kHz/8ch/32-bit is exceeded (1411 B/ms > 1023 B/ms limit).** Stage-5 32-bit mode at 44.1 kHz or 48 kHz enumerates as `malfunctioned` device. Valid combos: 8ch/32-bit at 16 kHz (512 B/ms) or 24 kHz (768 B/ms) only. At 44.1 kHz use 16-bit payload (8ch × 2B × 45 samples = 720 B/ms). Always calculate `channels × bytes_per_sample × ceil(sample_rate/1000)` before enabling wide-payload debug modes.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | SKILL C                        |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Persistent ~4 Hz AM artifact: variable-length USB packets + stream-start starvation.** After ring-buffer x32 and `last_good_sample` hold, spectrogram still showed a harmonic series at 4, 7, 10, 13 Hz (3 Hz spacing). Root cause A: on partial starvation (`frames_to_copy < usb_tx_frame_samples`), the old code shrunk `usb_tx_frame_samples` and sent a short USB packet. Windows accumulates variable-length packet drift and periodically bursts to compensate, creating a rhythmic AM. Root cause B: on alt=0->1 stream start, ring had zero warmup, making first callbacks vulnerable to Windows USB host scheduler stalls (~5-15 ms DPC latency spikes). Fix A: pad tail frames with `last_good_sample` instead of truncating — USB packet size always fixed. Fix B: arm a 20-callback pre-fill gate on alt=1 so ring accumulates ~20 ms before USB reads begin.                                                                                                        | SKILL C, FW-CRIT-2, FW-CRIT-7  |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **SD3 elimination from capture path.** With only 3 INMP441 mic pairs (SD0/SD1/SD2), SD3 was still configured as a PIO input in `gpio_configure()`, overriding the main.c `GPIO_OUT LOW` setup. Fix: configure `sd_base+3` as plain `GPIO_OUT LOW` in the I2S driver `gpio_configure()` and restrict `configure_sd_pads()` to SD0..SD2 only. PIO `in pins, 4` continues sampling all 4 bits but SD3 bit is now deterministically zero.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               | SKILL A, FW-CRIT-4             |
| 2026-04-04 | mic-array-pico2w-usb6ch                | **Ring buffer over-read: confirmed hardware root cause of residual ring-mod.** `microphone_array_i2s_read_stream_nonblocking` was called with `sizeof(buffer) = 1568 B` (49 frames x 32 B, sized for 48 kHz max rate). At 44.1 kHz, only 44-45 frames (1408-1440 B) are consumed. The extra ~160 B/ms is read from the ring and discarded after `decode_sample`. DMA fills at 1411 B/ms; USB drained at 1568 B/ms while ring had headroom, depleting the 28 KB pre-fill buffer in ~180 ms and silently restarting starvation. All intermediate fixes (last_good_sample hold, tail-fill, crossfade, pre-fill gate) were treating symptoms; this was the underlying cause. Fix: `request_bytes = usb_tx_frame_samples * (4 * I2S_RX_FRAME_SIZE_IN_BYTES)`. USB drain now matches DMA fill rate; ring stays full; ring-mod eliminated. PIO timing audit also performed: I2S WS/BCLK transitions correct, decode_sample bit-interleaving correct, actual rate 44,103 Hz (0.007% error). | SKILL C, FW-CRIT-15            |
| 2026-03-31 | mic-array-pico2w-usb6ch                | `pico2_w` board definition missing from SDK v2.0.0 — cmake error at configure time. Fix: upgrade to SDK v2.1.0. Not a code problem.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | SKILL B, SKILL D pre-check     |
| 2026-03-31 | mic-array-pico2w-usb6ch                | Denisgav library examples use TinyUSB old API (`tusb_init` with two args). SDK 2.1.0 bundles TinyUSB 0.16 which requires zero-arg `tusb_init()`. Fix: disable incompatible examples in CMakeLists; application already uses new API.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | SKILL B, SKILL D               |
| 2026-03-31 | mic-array-pico2w-usb6ch                | USB channel count 8 reported vs 6 physical mics. Design decision — 3 I2S data lanes × (L+R) = 6 active + 2 silent padding. Not a bug. Product string "8ch (6 active)" documents it.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | SKILL C channel count guidance |
| 2026-03-31 | mic-array-pico2w-usb6ch                | Windows PowerShell: `&&` is not valid between commands; `PICO_SDK_PATH` and toolchain PATH do not persist across sessions — must set per session. `-G Ninja` required for Windows CMake builds.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | SKILL B Windows section        |
| 2026-03-31 | mic-array-pico2w-usb6ch                | Pico SDK 2.1.0 shallow clone without submodules will not build pico2_w target. Requires explicit: `git submodule update --init --depth 1 lib/tinyusb lib/cyw43-driver lib/lwip`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | SKILL D pre-migration check    |
| 2026-03-31 | mic-array-pico2w-usb6ch                | SDK 2.1.0 auto-fetches picotool from git if not pre-installed. Adds ~2 min to first cmake configure. Normal behaviour, not a failure.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               | SKILL B                        |
| 2026-03-31 | mic-array-pico2w-usb6ch                | Plan used term `N_CHANNELS_RX` for mic channel count. Correct TinyUSB name is `N_CHANNELS_TX` — device transmits audio to host. `_RX` is for speaker/playback devices.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | SKILL C USB naming convention  |
| 2026-03-31 | mic-array-pico2w-usb6ch                | `PICO_PLATFORM` must never be set manually in CMakeLists. SDK auto-derives from `PICO_BOARD`. Setting it manually causes cmake build system conflicts.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | SKILL A Layer 1 rules          |

---

## VERSION HISTORY

| Version | Change                                                                                                                                                                                                                                                                                                                                                                                                  |
|:------- |:------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1.0     | Initial — 5 merged skills, CDE3301 project scope                                                                                                                                                                                                                                                                                                                                                        |
| 1.1     | Added Pico USB audio bring-up lessons: TinyUSB device init, Windows USB cache behavior, and staged UAC2 debug ladder                                                                                                                                                                                                                                                                                    |
| 1.2     | UAC2 descriptor/app alignment notes (`docs/UAC2_DESCRIPTOR_NOTES.md`): 32-bit USB PCM path, Stage 4 I2S init, restored TinyUSB TX order                                                                                                                                                                                                                                                                 |
| 1.3     | Added Windows host integration lessons: `8 exposed / 6 active` USB contract, Audacity vs PortAudio validation gap, split input/output sample rates, and explicit channel mute/remap patterns for Sound_Bubble realtime testing                                                                                                                                                                          |
| 1.4     | Added realtime fidelity lessons from Sound_Bubble: per-hop sequential inference requirement, block-duration throughput guardrail, and boundary-level instrumentation (`usb`/`mdl_in`/`model`) for separation debugging                                                                                                                                                                                  |
| 1.5     | Added Sound_Bubble training/integration guardrails: config-to-module contract verification (`num_noises` compatibility) and model-variant migration contract alignment (`model + dataset schema + realtime preset`)                                                                                                                                                                                     |
| 1.6     | Added environment and training-execution guardrails: integrated-terminal conda profile contract, distance-threshold-to-split alignment rule, and deterministic local run-dir preset fallback strategy                                                                                                                                                                                                   |
| 1.7     | Added audit-cycle lessons: shared runtime contract JSON across ONNX/export/benchmark/pipeline, rolling-window silence validation policy, 24 kHz native firmware support path, sample-rate-aware clock validation, explicit 12 mA I2S clock drive rule, DMA overrun stage-gate guidance, and RP2350 `SW_SPIN_TRY_LOCK` blocker reclassification                                                          |
| 1.8     | Added RP2350 build-root-cause refinement (`CMAKE_C_FLAGS` override hazard), clarified TinyUSB `OPT_MCU_RP2040` alias interpretation on RP2350, and documented runtime-selectable UAC2 sample-rate contract (16k/24k/44.1k/48k with build-time default only)                                                                                                                                             |
| 1.9     | Added stability correction: runtime UAC2 sample-rate switching is not production-stable on current Windows/Audacity path for this project; restored fixed-rate, read-only clock-control contract while preserving 24 kHz build-time selection and prior firmware telemetry/signal-integrity improvements                                                                                                |
| 2.0     | Added mandatory **Critical Firmware Safety Guardrails** section with hard-stop release gates to prevent host/device instability (USB callback blocking, mid-stream contract mutation, descriptor identity churn, and unsafe recovery behaviour).                                                                                                                                                        |
| 2.1     | Updated guardrails for selectable-rate UAC2 operation: enforced descriptor/control parity, discrete supported-rate ranges, and idle-apply/streaming-defer sample-rate transitions as mandatory safety contract.                                                                                                                                                                                         |
| 2.2     | Added **FW-CRIT-11** (buffer/endpoint sizing for max runtime rate in selectable-rate builds) and **FW-CRIT-12** (PID/bcd bump on any structural descriptor change). Added two audit-finding patterns documenting root causes of `-9996` Invalid device: undersized endpoint max-packet and stale Windows cached descriptor from PID not bumped on shape change.                                         |
| 2.3     | Added **FW-CRIT-13**: USB mic plug-in must never wedge or kill host system audio output (speakers/headphones). Strengthened Audio Coexistence Gate to require active plug-in test. Added field failure pattern from speaker lockup incident requiring reboot to recover.                                                                                                                                |
| 2.4     | Added **FW-CRIT-14**: DMA ring buffer must be ≥ 4× half-buffer (canonical: 8× DMA buffer). Diagnosed and documented root cause of ring-modulator/metallic distortion: `empty_dma()` drops every second half-buffer push when ring = 1× DMA buffer, producing ≈ 919 Hz AM at 44.1 kHz. Added SKILL C quick-reference entry for DMA distortion symptom. Updated SKILL E triage table.                     |
| 2.5     | **FW-CRIT-14 confirmed working in hardware.** Added three host-integration patterns from first-flash validation: Windows UAC2 default rate driven by `subrange[0]` order; Audacity -9996 workaround (WASAPI host + match project rate to Windows default); `picotool.exe` file-lock workaround during `-Rebuild` (`taskkill /F` or alternate build directory).                                          |
| 2.6     | Corrected FW-CRIT-14 with the final compounding-error diagnosis: ring buffer sizing must be anchored to `AUDIO_SAMPLE_RATE_MAX` and increased to 16 ms headroom. Documented v1/v2 root-cause progression and the implemented max-rate formula in discovered patterns.                                                                                                                                   |
| 2.7     | Confirmed ring-modulator root cause in hardware: zero-fill on short I2S reads creates hard silence edges → vertical spectrogram spikes → AM artifact. Fixed with `last_good_sample` continuity hold + DC-blocking exponential decay (`>> 4`). Added USB FS payload limit pattern (8ch/32-bit only valid at ≤ 24 kHz). Added SD3 elimination pattern (floating input overriding GPIO_OUT LOW in driver). |
| 2.8     | Diagnosed persistent ~4 Hz AM artifact as dual root cause: (A) partial-starvation truncated USB packets (variable packet length creates Windows compensation rhythm) and (B) zero-warmup ring at stream start vulnerable to DPC stalls. Fixed both: tail-fill with last_good_sample to keep USB packet size fixed; 20-callback pre-fill gate (ring_prefill_gate) on alt=0->1 transition.                |
| 2.9     | Identified ring buffer over-read as primary starvation driver at 44.1 kHz: request size 1568 B vs DMA fill 1411 B/ms drained pre-fill buffer in 180 ms. Fix: read exactly usb_tx_frame_samples x 32 bytes per callback. PIO clocking audit confirmed correct (44,103 Hz, 0.007% error). I2S WS/BCLK timing and decode_sample bit-interleaving also confirmed correct.                                   |
|         | 3.0                                                                                                                                                                                                                                                                                                                                                                                                     |
|         | 3.1                                                                                                                                                                                                                                                                                                                                                                                                     |
