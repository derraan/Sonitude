#pragma once

// PIO clock divider math for the PCM5101A I2S transmitter.
// Pure C, no SDK dependencies, so it can be unit-tested on the host.
//
// pio_freq = fs * PCM5101A_PIO_CYCLES_PER_FRAME (2 cycles/bit * 64 bits)
// div      = f_sys / pio_freq, encoded as INT + FRAC/256.
//
// At f_sys = 192 MHz (AUDIO_F_SYS_KHZ of the mic firmware) these are exact:
//   16 kHz -> 93 + 192/256, 24 kHz -> 62 + 128/256,
//   32 kHz -> 46 + 224/256, 48 kHz -> 31 +  64/256.

#include <stdbool.h>
#include <stdint.h>

#define PCM5101A_BCK_PER_FRAME        64u
#define PCM5101A_PIO_CYCLES_PER_FRAME (2u * PCM5101A_BCK_PER_FRAME)

typedef struct {
    uint16_t div_int;
    uint8_t div_frac;
    bool exact;
    int32_t error_ppb;  // (actual_fs - target_fs) / target_fs * 1e9
} pcm5101a_clkdiv_t;

// Sample rates the PCM5101A BCK PLL accepts with BCK = 64 fs
// (datasheet Table 11): 16, 32, 44.1, 48, 96 kHz; plus 24 kHz which
// falls in the auto-detected 16..48 kHz band.
static inline bool pcm5101a_rate_supported(uint32_t fs) {
    switch (fs) {
    case 16000: case 24000: case 32000: case 44100: case 48000: case 96000:
        return true;
    default:
        return false;
    }
}

static inline bool pcm5101a_compute_clkdiv(uint32_t f_sys_hz, uint32_t fs, pcm5101a_clkdiv_t *out) {
    if (fs == 0 || !pcm5101a_rate_supported(fs)) {
        return false;
    }
    uint64_t pio_hz = (uint64_t)fs * PCM5101A_PIO_CYCLES_PER_FRAME;
    uint64_t div256_num = (uint64_t)f_sys_hz * 256u;
    uint64_t div256 = (div256_num + pio_hz / 2) / pio_hz;
    if (div256 < 256u || div256 >= (65536ull << 8)) {
        return false;
    }
    out->div_int = (uint16_t)(div256 >> 8);
    out->div_frac = (uint8_t)(div256 & 0xffu);
    out->exact = (div256 * pio_hz) == div256_num;
    // actual_fs / fs - 1 = (f_sys*256 - div256*pio_hz) / (div256*pio_hz)
    int64_t den = (int64_t)(div256 * pio_hz);
    int64_t diff = (int64_t)div256_num - den;
    out->error_ppb = (int32_t)(diff * 1000000000ll / den);
    return true;
}
