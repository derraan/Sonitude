#pragma once

// Audio system clock and PIO divider constants for RP2350 (Pico 2W).
//
// System clock: 192 MHz
//   PLL config: FBDIV=64, POSTDIV1=2, POSTDIV2=2 → VCO=768 MHz
//   VCO 768 MHz is within the RP2350 spec range of 750–1800 MHz.
//   Call set_sys_clock_khz(AUDIO_F_SYS_KHZ, true) as the FIRST statement
//   in main(), before board_init() and any peripheral initialisation.
//
// USB PLL is independent (dedicated 48 MHz USBPLL). Changing the system
// clock does NOT affect USB enumeration or isochronous timing.
//
// PIO clock frequency formula (from microphone_array_i2s.c):
//   pio_freq = sample_rate × SAMPLES_PER_FRAME × 32 × PIO_INSTRUCTIONS_PER_BIT
//            = sample_rate × 2 × 32 × 2
//            = sample_rate × 128
// Each SCK cycle = 2 PIO cycles, so fSCK = pio_freq / 2.
// fWS = fSCK / 64 = sample_rate. ✓
//
// Divider notation: pio_sm_config_set_clkdiv_int_frac(&cfg, INT, FRAC)
//   actual_pio_freq = f_sys × 256 / (INT×256 + FRAC)
//   ppm = (actual − target) / target × 1e6
//
// INMP441 fSCK limits (DS-INMP441-00 Rev 1.1, Table 3):
//   min 0.5 MHz, max 3.2 MHz  →  sample rates 7.8 kHz – 50 kHz only.
//   96 kHz is NOT supported (fSCK = 6.144 MHz > 3.2 MHz max).

#define AUDIO_F_SYS_KHZ    192000u
#define AUDIO_F_SYS_HZ     192000000u

// Per-sample-rate PIO divider constants.
// These are only compiled when AUDIO_SAMPLE_RATE is already defined by the
// time this header is included. Define AUDIO_SAMPLE_RATE (e.g. as
// CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE from tusb_config.h) before including
// this header to get the AUDIO_PIO_DIV_INT/FRAC constants.
// AUDIO_F_SYS_KHZ is always available regardless.
#ifdef AUDIO_SAMPLE_RATE

// ─────────────────────────────────────────────────────────────────────────────
#if AUDIO_SAMPLE_RATE == 16000
// ─────────────────────────────────────────────────────────────────────────────
// pio_freq_target = 16000 × 128 = 2,048,000 Hz
// clkdiv_ideal    = 192,000,000 / 2,048,000 = 93.75 (exact)
// frac            = 0.75 × 256 = 192 (exact integer)
// actual pio_freq = 192,000,000 / 93.75 = 2,048,000 Hz
// fSCK actual     = 1,024,000 Hz   (= 16000 × 64 ✓)
// ppm error       = 0              (exact)
#define AUDIO_F_SCK_HZ          1024000u
#define AUDIO_PIO_DIV_INT          93u
#define AUDIO_PIO_DIV_FRAC        192u
#define AUDIO_F_SCK_ACTUAL      1024000u
#define AUDIO_PPM_ERROR                0

// ─────────────────────────────────────────────────────────────────────────────
#elif AUDIO_SAMPLE_RATE == 44100
// ─────────────────────────────────────────────────────────────────────────────
// pio_freq_target = 44,100 × 128 = 5,644,800 Hz
// clkdiv_ideal    = 192,000,000 / 5,644,800 = 34.01360…
// int=34, frac=round(0.01360 × 256) = round(3.48) = 3
// actual pio_freq = 192,000,000 × 256 / 8707 ≈ 5,645,113 Hz
// fSCK actual     ≈ 2,822,557 Hz
// ppm error       ≈ +55 ppm  ✓ (<100 ppm)
//
// 44.1 kHz cannot be derived exactly from a 12 MHz crystal via integer PLL
// dividers. 55 ppm is the minimum achievable residual at 192 MHz.
#define AUDIO_F_SCK_HZ          2822400u
#define AUDIO_PIO_DIV_INT          34u
#define AUDIO_PIO_DIV_FRAC          3u
#define AUDIO_F_SCK_ACTUAL      2822557u
#define AUDIO_PPM_ERROR              +55

// ─────────────────────────────────────────────────────────────────────────────
#elif AUDIO_SAMPLE_RATE == 24000
// ─────────────────────────────────────────────────────────────────────────────
// pio_freq_target = 24,000 × 128 = 3,072,000 Hz
// clkdiv_ideal    = 192,000,000 / 3,072,000 = 62.5 (exact)
// frac            = 0.5 × 256 = 128 (exact integer)
// actual pio_freq = 192,000,000 / 62.5 = 3,072,000 Hz
// fSCK actual     = 1,536,000 Hz   (= 24000 × 64 ✓)
// ppm error       = 0              (exact)
#define AUDIO_F_SCK_HZ          1536000u
#define AUDIO_PIO_DIV_INT          62u
#define AUDIO_PIO_DIV_FRAC        128u
#define AUDIO_F_SCK_ACTUAL      1536000u
#define AUDIO_PPM_ERROR                0

// ─────────────────────────────────────────────────────────────────────────────
#elif AUDIO_SAMPLE_RATE == 48000
// ─────────────────────────────────────────────────────────────────────────────
// pio_freq_target = 48,000 × 128 = 6,144,000 Hz
// clkdiv_ideal    = 192,000,000 / 6,144,000 = 31.25 (exact)
// frac            = 0.25 × 256 = 64 (exact integer)
// actual pio_freq = 192,000,000 / 31.25 = 6,144,000 Hz
// fSCK actual     = 3,072,000 Hz   (= 48000 × 64 ✓, within INMP441 3.2 MHz max ✓)
// ppm error       = 0              (exact)
#define AUDIO_F_SCK_HZ          3072000u
#define AUDIO_PIO_DIV_INT          31u
#define AUDIO_PIO_DIV_FRAC         64u
#define AUDIO_F_SCK_ACTUAL      3072000u
#define AUDIO_PPM_ERROR                0

#elif AUDIO_SAMPLE_RATE == 96000
// 96 kHz: fSCK = 6.144 MHz > INMP441 maximum of 3.2 MHz — hardware does NOT support this.
// Defining it here allows detection at link time if AUDIO_F_SCK_HZ is referenced,
// but it WILL NOT produce correct audio. Do not use 96 kHz with INMP441.
#define AUDIO_F_SCK_HZ          6144000u
#define AUDIO_PIO_DIV_INT          15u
#define AUDIO_PIO_DIV_FRAC        128u
#define AUDIO_F_SCK_ACTUAL      6144000u
#define AUDIO_PPM_ERROR                0
#endif  // AUDIO_SAMPLE_RATE value switch

#endif  // AUDIO_SAMPLE_RATE defined
