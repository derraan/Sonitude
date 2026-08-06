#pragma once


#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "microphone_array_i2s.pio.h"

#define MAX_I2S_RP2 (2)

// DMA buffer sizing notes:
// - Keep full buffer aligned to the active 6ch x 32-bit frame contract (24 bytes/frame).
// - Keep half-buffer aligned to 32-bit DMA words.
// - Keep ping-pong (double-buffer) behavior.
// 1536 bytes = 64 * 24-byte frames, half=768 bytes.
#define SIZEOF_DMA_BUFFER_IN_BYTES (1536)
#define SIZEOF_HALF_DMA_BUFFER_IN_BYTES (SIZEOF_DMA_BUFFER_IN_BYTES / 2)
#define I2S_NUM_DMA_CHANNELS (2)

#define NUM_I2S_USER_FORMATS (4)
#define I2S_RX_FRAME_SIZE_IN_BYTES (8)

#define SAMPLES_PER_FRAME (2)
#define PIO_INSTRUCTIONS_PER_BIT (2)

#ifndef STATIC
    #define STATIC static
#endif //STATIC

#ifndef m_new
    #define m_new(type, num) ((type *)(malloc(sizeof(type) * (num))))
#endif //m_new

#ifndef m_new_obj
    #define m_new_obj(type) (m_new(type, 1))
#endif //m_new_obj

#define mp_hal_pin_obj_t uint

typedef enum {
    GP_INPUT = 0,
    GP_OUTPUT = 1
} gpio_dir_t;

typedef enum {
    BLOCKING,
    NON_BLOCKING,
    UASYNCIO
} io_mode_t;

typedef struct __attribute__((packed)) {
    uint32_t sample_l[4];
    uint32_t sample_r[4];
} i2s_audio_sample;

typedef struct _ring_buf_t {
    uint8_t *buffer;
    size_t head;
    size_t tail;
    size_t size;
} ring_buf_t;

typedef struct _microphone_array_i2s_obj_t {
    uint8_t i2s_id;
    mp_hal_pin_obj_t sck_base;
    mp_hal_pin_obj_t sd_base;
    int8_t bits;
    int32_t rate;
    int32_t ibuf;
    io_mode_t io_mode;
    PIO pio;
    uint8_t sm;
    const pio_program_t *pio_program;
    uint prog_offset;
    int dma_channel[I2S_NUM_DMA_CHANNELS];
    uint8_t dma_buffer[SIZEOF_DMA_BUFFER_IN_BYTES];
    ring_buf_t ring_buffer;
    uint8_t *ring_buffer_storage;
    uint16_t pio_clkdiv_int;
    uint8_t pio_clkdiv_frac;
} microphone_array_i2s_obj_t;

// Buffer protocol
typedef struct _mp_buffer_info_t {
    void *buf;      // can be NULL if len == 0
    size_t len;     // in bytes
    int typecode;   // as per binary.h
} mp_buffer_info_t;


microphone_array_i2s_obj_t* create_microphone_array_i2s(uint8_t i2s_id,
              mp_hal_pin_obj_t sck_base, mp_hal_pin_obj_t sd_base,
              int8_t i2s_bits, int32_t ring_buffer_len, int32_t i2s_rate);


int microphone_array_i2s_read_stream(microphone_array_i2s_obj_t *self, void *buf_in, size_t size);
int microphone_array_i2s_read_stream_nonblocking(microphone_array_i2s_obj_t *self, void *buf_in, size_t size);

i2s_audio_sample decode_sample(i2s_audio_sample* sample_ptr_in);

// Stage 8: returns the error code from the last call to create_microphone_array_i2s.
// 0 = success; negative = failure (-1=no PIO SM, -2=no PIO space, -3=bad bits,
// -5=bad ring buf len, -6=malloc failed, -11=no DMA channels).
int8_t get_last_i2s_init_error(void);

// Stage 9: cumulative count of DMA IRQ fires since boot.
uint32_t get_i2s_dma_irq_count(void);

// Stage 11: cumulative count of dropped DMA half-buffers due to ring full.
uint32_t get_i2s_dma_overrun_count(void);

// Stage 11: cumulative bytes dropped due to DMA ring overflow.
uint32_t get_i2s_dma_overrun_bytes(void);