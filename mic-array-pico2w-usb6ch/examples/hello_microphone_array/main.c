/*
 * Copyright (c) 2021 Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 * 
 * This examples captures data from a I2S microphone using a sample
 * rate of 8 kHz and prints the sample values over the USB serial
 * connection.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/microphone_array_i2s.h"

#include "main.h"
#define SAMPLE_BUFFER_SIZE  (RATE/1000)

// Read data from tty:
// minicom --device /dev/ttyACM0

int main() {
    stdio_init_all();
    microphone_array_i2s_obj_t* i2s0 = create_microphone_array_i2s(0, PIN_SCK, PIN_SD0, BPS, SIZEOF_DMA_BUFFER_IN_BYTES, RATE);
    int32_t buffer[4*2*SAMPLE_BUFFER_SIZE];
    while (true) {
        int num_bytes_read = microphone_array_i2s_read_stream(i2s0, (void*)&buffer[0], sizeof(buffer));
        i2s_audio_sample* sample_ptr = (i2s_audio_sample*)buffer;
        int num_of_frames_read = num_bytes_read/(4*I2S_RX_FRAME_SIZE_IN_BYTES);
        for(int sample_idx = 0; sample_idx < num_of_frames_read; sample_idx++)
        {
            i2s_audio_sample sample = decode_sample(&(sample_ptr[sample_idx]));
            printf("%d %.8x %.8x %.8x %.8x %.8x %.8x %.8x %.8x\n", sample_idx,
                sample.sample_l[0], sample.sample_l[1], sample.sample_l[2], sample.sample_l[3],
                sample.sample_r[0], sample.sample_r[1], sample.sample_r[2], sample.sample_r[3]);
        }
    }
}

