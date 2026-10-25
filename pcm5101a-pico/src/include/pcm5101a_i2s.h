#pragma once

// PCM5101A line-out driver: 3-wire I2S (no SCK), PIO + chained ping-pong DMA.
//
// Single instance. Audio frames are interleaved stereo int32, MSB-aligned
// (a 16-bit sample s becomes (int32_t)s << 16; 24-bit becomes s << 8).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/pio.h"

// Frames per DMA half-buffer. 16 frames = 0.33 ms at 48 kHz.
#ifndef PCM5101A_BLOCK_FRAMES
#define PCM5101A_BLOCK_FRAMES 16u
#endif

// Software FIFO capacity in frames (power of two).
#ifndef PCM5101A_FIFO_FRAMES
#define PCM5101A_FIFO_FRAMES 128u
#endif

// DMA IRQ line. RP2350 has DMA_IRQ_2/3, which the mic driver does not use.
#ifndef PCM5101A_DMA_IRQ_INDEX
#if PICO_RP2350
#define PCM5101A_DMA_IRQ_INDEX 2
#else
#define PCM5101A_DMA_IRQ_INDEX 1
#endif
#endif

#define PCM5101A_PIN_UNUSED (-1)

typedef struct {
    PIO pio;               // PIO block; a free state machine is claimed
    uint pin_din;          // -> DIN  (pin 14)
    uint pin_bck;          // -> BCK  (pin 13); LRCK (pin 15) must be pin_bck + 1
    int pin_xsmt;          // -> XSMT (pin 17), or PCM5101A_PIN_UNUSED if strapped high
    int pin_flt;           // -> FLT  (pin 11), driven high (low latency), or PCM5101A_PIN_UNUSED
    uint32_t sample_rate;  // 16000, 24000, 32000, 44100, 48000 or 96000
} pcm5101a_config_t;

typedef struct {
    uint32_t irq_count;      // DMA blocks completed
    uint32_t underruns;      // blocks filled with silence (FIFO short)
    uint32_t overflows;      // pcm5101a_write calls that dropped frames
    uint32_t dropped_frames; // total frames rejected by pcm5101a_write
    uint32_t max_fill;       // highest FIFO level seen by the IRQ, frames
    int32_t clk_error_ppb;   // sample-rate error from PIO divider rounding
} pcm5101a_stats_t;

// Configures pins, PIO and DMA. Does not start clocks. Returns false on
// unsupported rate, pin layout, or if no SM / DMA channel is free.
bool pcm5101a_init(const pcm5101a_config_t *cfg);

// Starts BCK/LRCK with silence, waits for the DAC PLL to lock, then unmutes.
// Blocks for ~10 ms.
void pcm5101a_start(void);

// Soft-mutes, waits for the mute ramp, then stops clocks.
void pcm5101a_stop(void);

// Non-blocking. Returns frames accepted; the rest are dropped and counted.
size_t pcm5101a_write(const int32_t *lr, size_t frames);

// Frames currently queued in the software FIFO (excludes DMA buffers).
size_t pcm5101a_fill_level(void);

// Drives XSMT. No-op if pin_xsmt is unused.
void pcm5101a_set_mute(bool mute);

void pcm5101a_get_stats(pcm5101a_stats_t *out);
