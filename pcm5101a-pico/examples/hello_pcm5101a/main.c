// hello_pcm5101a: 1 kHz sine on the left channel, 500 Hz on the right.
// Every 10 s the producer pauses for 50 ms (forces underruns -> silence) and
// every 30 s the output is muted and restarted via pcm5101a_stop/start.
// Stats are printed over USB CDC once per second.

#include <math.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include "pcm5101a_i2s.h"

#ifndef AUDIO_F_SYS_KHZ
#define AUDIO_F_SYS_KHZ 192000u
#endif

// Carrier header (Pico GPIO). LRCK is the side-set pin next to BCK, so WS on
// GP2 forces BCK onto GP1. FLT is strapped on the board, not driven here.
#ifndef PIN_DAC_BCK
#define PIN_DAC_BCK 1    // GP1; LRCK/WS = GP2
#endif
#ifndef PIN_DAC_DIN
#define PIN_DAC_DIN 6    // GP6, SD out -> DIN
#endif
#ifndef PIN_DAC_XSMT
#define PIN_DAC_XSMT 7   // GP7, mute
#endif
#ifndef PIN_DAC_FLT
#define PIN_DAC_FLT PCM5101A_PIN_UNUSED
#endif
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 48000u
#endif

#define TABLE_LEN 48u  // 1 kHz at 48 kHz
#define CHUNK_FRAMES 16u
#define TARGET_FILL (2u * CHUNK_FRAMES)
#define AMPLITUDE 0.25f

static int32_t sine[TABLE_LEN];

int main(void) {
    set_sys_clock_khz(AUDIO_F_SYS_KHZ, true);
    stdio_init_all();

    for (uint i = 0; i < TABLE_LEN; i++) {
        sine[i] = (int32_t)(AMPLITUDE * 2147483647.0f * sinf(2.0f * (float)M_PI * (float)i / (float)TABLE_LEN));
    }

    const pcm5101a_config_t cfg = {
        .pio = pio1,
        .pin_din = PIN_DAC_DIN,
        .pin_bck = PIN_DAC_BCK,
        .pin_xsmt = PIN_DAC_XSMT,
        .pin_flt = PIN_DAC_FLT,
        .sample_rate = SAMPLE_RATE,
    };
    if (!pcm5101a_init(&cfg)) {
        while (true) {
            printf("pcm5101a_init failed\n");
            sleep_ms(1000);
        }
    }
    pcm5101a_start();

    uint32_t phase_l = 0, phase_r = 0;
    int32_t chunk[2 * CHUNK_FRAMES];
    absolute_time_t next_print = make_timeout_time_ms(1000);
    uint32_t seconds = 0;

    while (true) {
        while (pcm5101a_fill_level() < TARGET_FILL) {
            for (uint i = 0; i < CHUNK_FRAMES; i++) {
                chunk[2 * i] = sine[phase_l];
                chunk[2 * i + 1] = sine[phase_r / 2];
                phase_l = (phase_l + 1) % TABLE_LEN;
                phase_r = (phase_r + 1) % (2 * TABLE_LEN);
            }
            pcm5101a_write(chunk, CHUNK_FRAMES);
        }

        if (time_reached(next_print)) {
            next_print = delayed_by_ms(next_print, 1000);
            seconds++;
            pcm5101a_stats_t s;
            pcm5101a_get_stats(&s);
            printf("t=%lus irq=%lu underruns=%lu overflows=%lu max_fill=%lu clk_err=%ldppb\n",
                   (unsigned long)seconds, (unsigned long)s.irq_count, (unsigned long)s.underruns,
                   (unsigned long)s.overflows, (unsigned long)s.max_fill, (long)s.clk_error_ppb);

            if (seconds % 30 == 0) {
                pcm5101a_stop();
                sleep_ms(200);
                pcm5101a_start();
            } else if (seconds % 10 == 0) {
                sleep_ms(50);
            }
        }
    }
}
