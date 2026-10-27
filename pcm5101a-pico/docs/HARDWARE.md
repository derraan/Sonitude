# PCM5101A carrier hardware connections

Board-verified pin map for the Sonitude carrier (Pico 2 W + PCM5101A line-out).
Source: carrier PCB board view + PCM5101A datasheet (3-wire / hardware-pin mode).

## Signal map

| Role | Pico GPIO | Pico pin | Board silk | DAC pin | Notes |
| --- | --- | --- | --- | --- | --- |
| BCK | GP1 | 2 | — | BCK (13) | 64 × fs; PIO side-set base |
| WS / LRCK | GP2 | 4 | `AUDIO_WS` | LRCK (15) | Must be `pin_bck + 1` |
| SD out | GP6 | 9 | — | DIN (14) | MSB-first I2S data |
| Mute | GP7 | 10 | `MUTE` | XSMT (17) | Fast edge under 20 ns |

```
                    ┌─────────────┐
   GP1 (pin 2) ────►│ BCK      13 │
   GP2 (pin 4) ────►│ LRCK     15 │  AUDIO_WS
   GP6 (pin 9) ────►│ DIN      14 │
   GP7 (pin 10) ───►│ XSMT     17 │  MUTE
              GND ─►│ SCK      12 │  BCK PLL mode
             3.3 V ─►│ FLT      11 │  low-latency filter
              GND ─►│ FMT      16 │  I2S
              GND ─►│ DEMP     10 │
             3.3 V ─►│ AVDD/DVDD/CPVDD
                    │ OUTL / OUTR │──► line out (RC filter)
                    └─────────────┘
```

## Firmware defaults

[`examples/hello_pcm5101a/main.c`](../examples/hello_pcm5101a/main.c):

| Macro | Value |
| --- | --- |
| `PIN_DAC_BCK` | `1` (GP1; LRCK = GP2) |
| `PIN_DAC_DIN` | `6` (GP6) |
| `PIN_DAC_XSMT` | `7` (GP7) |
| `PIN_DAC_FLT` | `PCM5101A_PIN_UNUSED` (strapped on board) |

## Electrical notes

- 3-wire I2S only: no master clock from the Pico.
- Soft mute: drive XSMT low at power-up / stop; raise after ≥ 16 LRCK + PLL lock (~10 ms in the driver).
- Line out is 2.1 VRMS into ≥ 1 kΩ. Use a headphone amp for low-Z cans.
- Recommended output filter: 470 Ω series + 2.2 nF to GND per channel.

## Conflict with mic capture defaults

| Bus | GPIOs |
| --- | --- |
| Mic I2S (usb_microphone_array_6ch defaults) | GP0 SCK, GP1 WS, GP2–4 SD |
| DAC I2S (this package) | GP1 BCK, GP2 WS, GP6 DIN, GP7 mute |

GP1 and GP2 are shared. Do not run both drivers on those pins until the
UAC2 integration picks a single clock/pin plan.
