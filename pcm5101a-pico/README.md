# pcm5101a-pico

Standalone Pico SDK library that streams stereo PCM to a TI PCM5101A DAC
(line out) over 3-wire I2S, using one PIO state machine and two chained DMA
channels. Built for the Pico 2 W (RP2350) at the 192 MHz system clock used by
`mic-array-pico2w-usb6ch`, but has no dependency on that firmware.

## Wiring and pin straps (hardware mode, no I2C)

| PCM5101A pin | Connect to | Why |
| --- | --- | --- |
| BCK (13) | GPIO `pin_bck` (example: 10) | Bit clock, 64 fs |
| LRCK (15) | GPIO `pin_bck + 1` (example: 11) | Word clock, left = low |
| DIN (14) | GPIO `pin_din` (example: 12) | Data, MSB first, I2S |
| SCK (12) | **GND** | Enables the BCK PLL (3-wire mode) |
| XSMT (17) | GPIO `pin_xsmt` (example: 13), or 3.3 V | Soft mute control, needs fast edges (< 20 ns) |
| FLT (11) | GPIO `pin_flt` driven high (example: 14), or 3.3 V | Low-latency filter, 3.5 samples of delay instead of 22 |
| FMT (16) | GND | I2S format |
| DEMP (10) | GND | De-emphasis off |
| AVDD, CPVDD, DVDD | 3.3 V | Single supply, 3.3 V I/O matches the RP2350 |
| OUTL / OUTR | 470 Ω series + 2.2 nF to GND | Datasheet output filter; load must be at least 1 kΩ |

The PCM5101A is a line-level driver. Low-impedance headphones (16 to 32 Ω)
need a headphone amplifier after OUTL/OUTR.

## Supported sample rates

16, 24, 32, 44.1, 48 and 96 kHz, all with BCK = 64 fs, which the DAC's BCK
PLL requires at 16 kHz. At 192 MHz the PIO divider is exact for every rate
except 44.1 kHz (error under 100 ppm, reported in `clk_error_ppb`).

## API

```c
#include "pcm5101a_i2s.h"

pcm5101a_config_t cfg = {
    .pio = pio1, .pin_din = 12, .pin_bck = 10,
    .pin_xsmt = 13, .pin_flt = 14, .sample_rate = 48000,
};
pcm5101a_init(&cfg);   // pins, PIO, DMA; clocks not running yet
pcm5101a_start();      // clocks on with silence, ~10 ms PLL lock wait, unmute
pcm5101a_write(lr, n); // interleaved int32 L,R, MSB-aligned; non-blocking
pcm5101a_fill_level(); // frames queued in the software FIFO
pcm5101a_stop();       // mute, wait for the ramp, stop clocks
```

Samples are 32-bit, MSB-aligned: pass a 16-bit sample as `(int32_t)s << 16`.

Compile-time options (define them before the build):

- `PCM5101A_BLOCK_FRAMES` (default 16): DMA block size. Smaller blocks lower latency but raise the interrupt rate.
- `PCM5101A_FIFO_FRAMES` (default 128, must be a power of two): software FIFO capacity.
- `PCM5101A_DMA_IRQ_INDEX` (default 2 on RP2350, 1 on RP2040): which DMA interrupt line the driver uses.

## How it stays stable

- The two DMA channels chain into each other, so the clocks never stop
  between blocks. Stopping BCK/LRCK would make the DAC re-lock its PLL and
  mute.
- The interrupt handler runs from RAM at the highest priority. It refills the
  block that just finished. If the FIFO holds less than a full block, it sends
  silence for that block and counts an underrun instead of stalling the clocks.
- The FIFO is lock-free with one producer and one consumer, so `pcm5101a_write()`
  can be called from either core or from a USB callback.
- Start and stop follow the datasheet. XSMT stays low until the PLL has locked
  (at least 16 LRCK periods plus margin), and goes low for 150 samples + 0.2 ms
  before the clocks stop. This avoids pops.
- Counters (`pcm5101a_get_stats`): underruns, overflows, dropped frames, interrupt count, peak FIFO level.

## Latency at 48 kHz with the defaults

| Stage | Delay |
| --- | --- |
| DMA blocks (1 to 2 × 16 frames) | 0.33 to 0.67 ms |
| PIO TX FIFO (8 words) | 0.08 ms |
| DAC low-latency filter (3.5 samples) | 0.07 ms |
| Software FIFO | Whatever level the producer keeps (the example holds about 32 frames, 0.67 ms) |

## Build

Standalone (library plus the `hello_pcm5101a` tone example):

```bash
export PICO_SDK_PATH=/path/to/pico-sdk   # 2.1.0
cmake -S pcm5101a-pico -B build -DPICO_BOARD=pico2_w
cmake --build build --target hello_pcm5101a
```

`hello_pcm5101a` plays 1 kHz on the left and 500 Hz on the right. Every 10 s
it starves the FIFO for 50 ms to exercise underrun handling, and every 30 s it
runs stop/start. It prints stats over USB serial once a second.

Host tests (FIFO, divider math, and a cycle model of the PIO program checked
against a reference I2S receiver):

```bash
./pcm5101a-pico/tests/run_host_tests.sh
```

The cycle model in `tests/test_host.c` mirrors `src/pcm5101a_i2s.pio` and must
be updated with it.

## Hardware check

With a scope on BCK and LRCK: 3.072 MHz and 48 kHz. The tone should be clean,
with no click at start/stop, and `underruns` should only increase during the
50 ms pauses.

## Using it from another firmware project

```cmake
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../pcm5101a-pico ${CMAKE_BINARY_DIR}/pcm5101a-pico)
target_link_libraries(<your_target> PRIVATE pcm5101a_i2s)
```

When pulled in this way, only the library target is defined. The host project
must call `set_sys_clock_khz()`, since the driver reads `clk_sys` and does not
set it. The driver claims a free PIO state machine and DMA channels
dynamically, and registers a shared interrupt handler on its own DMA interrupt
line. On RP2350 that line is DMA_IRQ_2, which the mic driver does not use.
