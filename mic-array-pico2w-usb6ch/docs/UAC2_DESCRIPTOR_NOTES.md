# UAC2 descriptor alignment (mic-array-pico2w-usb6ch)

This document compares the RP2350 firmware with the original RP2040 `microphone-array-library-for-pico` example and records the **escalation ladder** for channel count.

## Original RP2040 reference (`inmp441_6ch_usb`)

| Item                                         | Value                                                                                                                                         |
| -------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE`           | 16000                                                                                                                                         |
| `CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX`         | 8                                                                                                                                             |
| `CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX` | **4** (32-bit PCM slots in UAC2 Type-I)                                                                                                       |
| Descriptor macro                             | `TUD_AUDIO_MIC_EIGHT_CH_DESCRIPTOR`                                                                                                           |
| USB TX buffers                               | `uint32_t i2s_dummy_buffer[...]`                                                                                                              |
| Sample conversion                            | `i2s_to_usb_32b_sample_convert` (pass-through `uint32_t`)                                                                                     |
| Streaming order                              | `on_usb_microphone_tx_pre_load` pushes FIFO filled **previously** by `on_usb_microphone_tx_post_load` (post-load reads I2S and fills buffers) |

## Fixes applied in this repo (RP2350)

1. **`CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX = 4`** — Restored to match the original so the UAC2 Type-I descriptor, TinyUSB audio driver, and application buffers agree (was 2 bytes / 16-bit only in the app path).

2. **`uint32_t` `i2s_dummy_buffer`** — Matches the 4-byte-per-channel USB contract.

3. **Pre/post load split** — Restored the RP2040 pattern: **post-load** calls `usb_audio_post_load_fill()` (I2S read + pack); **pre-load** only calls `tud_audio_write_support_ff` with the byte count derived from `usb_tx_frame_samples` (variable 44/45 at 44.1 kHz via `get_usb_frame_samples()`).

4. **Stage 4 I2S init** — `refresh_i2s_connections()` is now called when `USB_AUDIO_DEBUG_STAGE >= 4` (was `>= 5`, so Stage 4 never created `i2s0`).

5. **Stage 4 vs 5/6 mux** — Stage 4 (`USB_AUDIO_DEBUG_STAGE == 4`) fills **two** logical USB channels only; Stage 5+ fills six decoded mics across four FIFO rows (8 USB channels).

## Windows validation checklist

After flashing:

1. **Device Manager / Sound** — Device shows the expected channel count (2 for Stage 4, 8 for Stage 5/6).
2. **While recording** — In firmware builds with `USB_AUDIO_DEBUG_STAGE >= 4`, debugger-visible counters: `debug_s6_tx_pre_count` / `debug_s6_tx_post_count` should increase; `debug_s6_cur_alt` should become **1** when streaming (not stuck at 0).
3. **Host script** — `test_usb_audio.py` defaults to `int16`. If the host stacks the device as 32-bit PCM, use `--dtype int32` (see script help).

## Escalation ladder (channels)

| Step       | Stage | `USB_AUDIO_DEBUG_STAGE` | USB logical channels | Sample rate (current `tusb_config.h`) |
| ---------- | ----- | ----------------------- | -------------------- | ------------------------------------- |
| Baseline   | 4     | 4                       | 2                    | 16000 Hz                              |
| Full array | 5     | 5                       | 8 (6 active)         | 44100 Hz                              |
| Debug      | 6     | 6                       | 8 + CDC              | 44100 Hz                              |

All stages use **32-bit PCM slots** on the USB side (`N_BYTES_PER_SAMPLE_TX == 4`), matching the RP2040 reference.

To move from 2ch → 8ch: rebuild with `-DUSB_AUDIO_DEBUG_STAGE=5`, reflash, uninstall old USB audio device entry if Windows caches descriptors (distinct PID per stage already provided in `usb_descriptors.c`).

## Optional future work

- **4-channel intermediate** — Not implemented; add a `TUD_AUDIO_MIC_*` descriptor with four logical channels or duplicate the 8ch descriptor with unused channels zeroed if Windows rejects odd layouts.
- **`stdio_flush` in `ringbuf_pop`** — Remove or replace with a no-op in IRQ context (see `microphone_array_i2s.c`).
