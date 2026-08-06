# mic-array-pico2w-usb6ch

6-channel USB microphone array firmware for **Raspberry Pi Pico 2W (RP2350)**.

Migrated from RP2040 (Pico) to RP2350 (Pico 2W) as part of the CDE3301 capstone project.

---

## Hardware

| Component | Part |
|:--- |:--- |
| Microcontroller | Raspberry Pi Pico 2W (RP2350, Cortex-M33) |
| Microphone array | DFRobot 6+1 INMP441 I2S MEMS Microphone Array |
| Interface | USB Full-Speed, UAC2 audio class |

### Pin Assignment

| Signal | GPIO |
|:--- |:--- |
| I2S SCK (clock) | GPIO 0 |
| I2S WS (word select) | GPIO 1 |
| I2S SD0 (data lane 0) | GPIO 2 |
| I2S SD1 (data lane 1) | GPIO 3 |
| I2S SD2 (data lane 2) | GPIO 4 |
| I2S SD3 (unused, pulled low) | GPIO 5 |

---

## Audio Configuration

| Parameter | Value |
|:--- |:--- |
| USB Audio Class | UAC2 (bcdADC = 0x0200) |
| USB channel count | 8 (channels 1-6 active, 7-8 silent) |
| Sample rate | 16000 Hz |
| Bit depth | 32 bits per sample |
| Product string | "INMP441 8ch (6 active)" |

> **Note:** 8 channels are reported to the USB host to match the DFRobot array's
> physical I2S pin layout (3 data lanes, each carrying L+R = 6 active + 2 silent).

---

## Three-Layer Software Architecture

```
┌─────────────────────────────────────────────────────────┐
│  LAYER 3 — APPLICATION LAYER                            │
│  examples/usb_microphone_array_6ch/                     │
│  main.c, usb_descriptors.c, tusb_config.h               │
│  Custom 6-mic USB logic, volume control, I2S routing    │
├─────────────────────────────────────────────────────────┤
│  LAYER 2 — PLATFORM LAYER                               │
│  src/microphone_array_i2s.c/.h, src/volume_ctrl.c       │
│  PIO I2S driver, DMA ring buffer, TinyUSB UAC2 base     │
│  Clock divider: clock_get_hz(clk_sys) — hardware agnostic│
├─────────────────────────────────────────────────────────┤
│  LAYER 1 — HARDWARE ABSTRACTION LAYER                   │
│  CMakeLists.txt: set(PICO_BOARD pico2_w)                │
│  Pico SDK v2.1.0, RP2350, Cortex-M33 @ 150 MHz         │
└─────────────────────────────────────────────────────────┘
```

---

## Build Requirements

| Dependency | Version / Notes |
|:--- |:--- |
| Pico SDK | v2.1.0+ (pico2_w board added in v2.1.0) |
| arm-none-eabi-gcc | 13.2 Rel1 (or compatible) |
| CMake | >= 3.13 |
| Ninja | Any recent version |

Set environment variable before building:
```powershell
$env:PICO_SDK_PATH = "C:\path\to\pico-sdk-2.1.0"
$env:PICO_TOOLCHAIN_PATH = "C:\path\to\arm-none-eabi-toolchain"
```

---

## Build Steps

```powershell
# From repo root
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -G Ninja ..
cmake --build . -- -j4
```

**Expected output confirmation lines from cmake:**
```
Pico Platform (PICO_PLATFORM) is 'rp2350-arm-s'.
Defaulting compiler (PICO_COMPILER) to 'pico_arm_cortex_m33_gcc' since not specified.
```

**Expected artefact:**
```
build/examples/usb_microphone_array_6ch/usb_mic_array_6ch_pico2w.uf2
```

---

## Flash and Enumerate Verification

### Step 1 — Enter BOOTSEL mode
1. Hold the **BOOTSEL** button on Pico 2W
2. Connect USB to PC while holding BOOTSEL
3. Release BOOTSEL

### Step 2 — Verify drive label
- Drive must appear as **`RP2350`**
- If it appears as `RPI-RP2`, the board target is wrong — check `PICO_BOARD=pico2_w`

### Step 3 — Flash
```powershell
# Copy UF2 to the RP2350 drive (adjust drive letter as needed)
Copy-Item "build\examples\usb_microphone_array_6ch\usb_mic_array_6ch_pico2w.uf2" "E:\"
```

### Step 4 — Enumerate and verify
| Check | Pass Condition |
|:--- |:--- |
| Windows Device Manager | USB Audio Device under Sound controllers, no yellow warning |
| Audacity input | "INMP441 8ch (6 active)" selectable |
| Channels 1-6 | Non-zero waveform when mic is stimulated |
| Channels 7-8 | Silent (by design — no physical mic on these lanes) |
| Sample rate | 16000 Hz |

---

## Known Issues

### Non-deterministic spinlock error on RP2350
**Symptom:** Build occasionally fails with a spinlock-related linker error.
**Fix:** Re-run `cmake --build .` without any code changes. Self-resolves on second attempt.
**Do not modify code** — this is an RP2350 SDK-level non-determinism, not a logic bug.

### Original denisgav examples disabled
The `hello_microphone_array`, `usb_microphone_array`, `usb_microphone_array_led`, and `sk9822`
examples are commented out in `CMakeLists.txt`. They were written against TinyUSB <0.16 API
and are incompatible with SDK 2.1.0's bundled TinyUSB 0.16+. Fixing them is out of scope.

---

## Layer Integrity Rules (from `skills_planner.md`)

- **Never** hardcode `125000000`, `125.0f`, or any clock frequency in Layer 2 or 3.
  Always use `clock_get_hz(clk_sys)`.
- **Never** set `PICO_BOARD`, `PICO_PLATFORM`, or toolchain paths in Layer 2 or 3.
  Board target is set only in `CMakeLists.txt` (Layer 1).
- **Never** set `PICO_PLATFORM` manually — SDK auto-derives it from `PICO_BOARD`.

---

## Git History

| Tag | Meaning |
|:--- |:--- |
| `upstream-rp2040-baseline` | Clean clone of denisgav repo before any changes |
| `pico2w-migration-complete-v1.0` | Migration complete, build verified |

---

## Credits

Platform layer based on [denisgav/microphone-array-library-for-pico](https://github.com/denisgav/microphone-array-library-for-pico).
Application layer built as part of **CDE3301 Capstone Project**, 2026.
