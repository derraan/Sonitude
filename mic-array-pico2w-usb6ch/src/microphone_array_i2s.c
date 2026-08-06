#include "pico/microphone_array_i2s.h"
#include "hardware/regs/addressmap.h"  // PADS_BANK0_BASE (RP2350: 0x40038000)
#include <assert.h>

STATIC microphone_array_i2s_obj_t* microphone_array_i2s_obj[MAX_I2S_RP2] = {NULL, NULL};

// Stage 8: init error code (last failure from microphone_array_i2s_init_helper)
static int8_t _last_i2s_init_error = 0;

int8_t get_last_i2s_init_error(void) {
    return _last_i2s_init_error;
}

// Stage 9: DMA IRQ fire counter
static volatile uint32_t _i2s_dma_irq_count = 0;
static volatile uint32_t _i2s_dma_overrun_count = 0;
static volatile uint32_t _i2s_dma_overrun_bytes = 0;

uint32_t get_i2s_dma_irq_count(void) {
    return _i2s_dma_irq_count;
}

uint32_t get_i2s_dma_overrun_count(void) {
    return _i2s_dma_overrun_count;
}

uint32_t get_i2s_dma_overrun_bytes(void) {
    return _i2s_dma_overrun_bytes;
}

// The frame map is used with the readinto() method to transform the audio sample data coming
// from DMA memory (32-bit stereo) to the format specified
// in the I2S constructor.  e.g.  16-bit mono
STATIC const int8_t i2s_frame_map[NUM_I2S_USER_FORMATS][I2S_RX_FRAME_SIZE_IN_BYTES] = {
    {-1, -1,  0,  1, -1, -1, -1, -1 },  // Mono, 16-bits
    { 0,  1,  2,  3, -1, -1, -1, -1 },  // Mono, 32-bits
    {-1, -1,  0,  1, -1, -1,  2,  3 },  // Stereo, 16-bits
    { 0,  1,  2,  3,  4,  5,  6,  7 },  // Stereo, 32-bits
};

// RP2350 has 3 PIO blocks; RP2040 has 2. Guard the initialiser list to match
// NUM_PIOS at build time — a missing entry would zero-initialise to NULL and
// crash on the first iteration that lands on the third PIO slot.
#if NUM_PIOS >= 3
STATIC const PIO pio_instances[NUM_PIOS] = {pio0, pio1, pio2};
#else
STATIC const PIO pio_instances[NUM_PIOS] = {pio0, pio1};
#endif

STATIC uint8_t dma_get_bits(int8_t bits);
STATIC void dma_irq0_handler(void);
STATIC void dma_irq1_handler(void);
STATIC void microphone_array_i2s_deinit(microphone_array_i2s_obj_t *self);

// Ring Buffer
// Thread safe when used with these constraints:
// - Single Producer, Single Consumer
// - Sequential atomic operations
// One byte of capacity is used to detect buffer empty/full

STATIC void ringbuf_init(ring_buf_t *rbuf, uint8_t *buffer, size_t size) {
    rbuf->buffer = buffer;
    rbuf->size = size;
    rbuf->head = 0;
    rbuf->tail = 0;
}

STATIC bool ringbuf_push(ring_buf_t *rbuf, uint8_t data) {
    size_t next_tail = (rbuf->tail + 1) % rbuf->size;

    if (next_tail != rbuf->head) {
        rbuf->buffer[rbuf->tail] = data;
        rbuf->tail = next_tail;
        return true;
    }

    // full
    return false;
}

STATIC bool ringbuf_pop(ring_buf_t *rbuf, uint8_t *data) {
    if (rbuf->head == rbuf->tail) {
        // empty
        return false;
    }

    *data = rbuf->buffer[rbuf->head];
    rbuf->head = (rbuf->head + 1) % rbuf->size;
    return true;
}

STATIC bool ringbuf_is_empty(ring_buf_t *rbuf) {
    return rbuf->head == rbuf->tail;
}

STATIC bool ringbuf_is_full(ring_buf_t *rbuf) {
    return ((rbuf->tail + 1) % rbuf->size) == rbuf->head;
}

STATIC size_t ringbuf_available_data(ring_buf_t *rbuf) {
    return (rbuf->tail - rbuf->head + rbuf->size) % rbuf->size;
}

STATIC size_t ringbuf_available_space(ring_buf_t *rbuf) {
    return rbuf->size - ringbuf_available_data(rbuf) - 1;
}

STATIC int8_t get_frame_mapping_index(int8_t bits) {
    return 3;
}

STATIC uint32_t fill_appbuf_from_ringbuf(microphone_array_i2s_obj_t *self, mp_buffer_info_t *appbuf) {

    // copy audio samples from the ring buffer to the app buffer
    // loop, copying samples until the app buffer is filled
    // For uasyncio mode, the loop will make an early exit if the ring buffer becomes empty
    // Example:
    //   a MicroPython I2S object is configured for 16-bit mono (2 bytes per audio sample).
    //   For every frame coming from the ring buffer (8 bytes), 2 bytes are "cherry picked" and
    //   copied to the supplied app buffer.
    //   Thus, for every 1 byte copied to the app buffer, 4 bytes are read from the ring buffer.
    //   If a 8kB app buffer is supplied, 32kB of audio samples is read from the ring buffer.

    uint32_t num_bytes_copied_to_appbuf = 0;
    uint8_t *app_p = (uint8_t *)appbuf->buf;
    uint8_t appbuf_sample_size_in_bytes = (self->bits == 16? 2 : 4) * 2;
    uint32_t num_bytes_needed_from_ringbuf = appbuf->len * (I2S_RX_FRAME_SIZE_IN_BYTES / appbuf_sample_size_in_bytes);
    uint8_t discard_byte;
    // poll the ringbuf until a sample becomes available,  copy into appbuf using the mapping transform
    int a_index = 0;
    while (a_index < num_bytes_needed_from_ringbuf) {
        while (ringbuf_pop(&self->ring_buffer, app_p+a_index) == false) {
            ;
        }
        a_index++;
        num_bytes_copied_to_appbuf++;
    }
//     while (num_bytes_needed_from_ringbuf) {

//         uint8_t f_index = get_frame_mapping_index(self->bits);

//         for (uint8_t i = 0; i < I2S_RX_FRAME_SIZE_IN_BYTES; i++) {
//             int8_t r_to_a_mapping = i2s_frame_map[f_index][i];
//             if (r_to_a_mapping != -1) {
//                 if (self->io_mode == BLOCKING) {
//                     // poll the ringbuf until a sample becomes available,  copy into appbuf using the mapping transform
//                     while (ringbuf_pop(&self->ring_buffer, app_p + r_to_a_mapping) == false) {
//                         ;
//                     }
//                     num_bytes_copied_to_appbuf++;
//                 } else if (self->io_mode == UASYNCIO) {
//                     if (ringbuf_pop(&self->ring_buffer, app_p + r_to_a_mapping) == false) {
//                         // ring buffer is empty, exit
//                         goto exit;
//                     } else {
//                         num_bytes_copied_to_appbuf++;
//                     }
//                 } else {
//                     return 0;  // should never get here (non-blocking mode does not use this function)
//                 }
//             } else { // r_a_mapping == -1
//                 // discard unused byte from ring buffer
//                 if (self->io_mode == BLOCKING) {
//                     // poll the ringbuf until a sample becomes available
//                     while (ringbuf_pop(&self->ring_buffer, &discard_byte) == false) {
//                         ;
//                     }
//                 } else if (self->io_mode == UASYNCIO) {
//                     if (ringbuf_pop(&self->ring_buffer, &discard_byte) == false) {
//                         // ring buffer is empty, exit
//                         goto exit;
//                     }
//                 } else {
//                     return 0;  // should never get here (non-blocking mode does not use this function)
//                 }
//             }
//             num_bytes_needed_from_ringbuf--;
//         }
//         app_p += appbuf_sample_size_in_bytes;
//     }
// exit:
    return num_bytes_copied_to_appbuf;
}

STATIC uint32_t copy_appbuf_to_ringbuf(microphone_array_i2s_obj_t *self, mp_buffer_info_t *appbuf) {

    // copy audio samples from the app buffer to the ring buffer
    // loop, reading samples until the app buffer is emptied
    // for uasyncio mode, the loop will make an early exit if the ring buffer becomes full

    uint32_t a_index = 0;

    while (a_index < appbuf->len) {
        if (self->io_mode == BLOCKING) {
            // copy a byte to the ringbuf when space becomes available
            while (ringbuf_push(&self->ring_buffer, ((uint8_t *)appbuf->buf)[a_index]) == false) {
                ;
            }
            a_index++;
        } else if (self->io_mode == UASYNCIO) {
            if (ringbuf_push(&self->ring_buffer, ((uint8_t *)appbuf->buf)[a_index]) == false) {
                // ring buffer is full, exit
                break;
            } else {
                a_index++;
            }
        } else {
            return 0;  // should never get here (non-blocking mode does not use this function)
        }
    }

    return a_index;
}

// function is used in IRQ context
STATIC void empty_dma(microphone_array_i2s_obj_t *self, uint8_t *dma_buffer_p) {
    // when space exists, copy samples into ring buffer
    if (ringbuf_available_space(&self->ring_buffer) >= SIZEOF_HALF_DMA_BUFFER_IN_BYTES) {
        for (uint32_t i = 0; i < SIZEOF_HALF_DMA_BUFFER_IN_BYTES; i++) {
            ringbuf_push(&self->ring_buffer, dma_buffer_p[i]);
        }
    } else {
        // Ring is full; this half-buffer is dropped. Track explicitly for telemetry.
        _i2s_dma_overrun_count++;
        _i2s_dma_overrun_bytes += SIZEOF_HALF_DMA_BUFFER_IN_BYTES;
    }
}

// function is used in IRQ context
STATIC void feed_dma(microphone_array_i2s_obj_t *self, uint8_t *dma_buffer_p) {
    // when data exists, copy samples from ring buffer
    if (ringbuf_available_data(&self->ring_buffer) >= SIZEOF_HALF_DMA_BUFFER_IN_BYTES) {

        // copy a block of samples from the ring buffer to the dma buffer.
        // STM32 HAL API has a stereo I2S implementation, but not mono
        // mono format is implemented by duplicating each sample into both L and R channels.
        
        // STEREO, both 16-bit and 32-bit
        for (uint32_t i = 0; i < SIZEOF_HALF_DMA_BUFFER_IN_BYTES; i++) {
            ringbuf_pop(&self->ring_buffer, &dma_buffer_p[i]);
        }
    } else {
        // underflow.  clear buffer to transmit "silence" on the I2S bus
        memset(dma_buffer_p, 0, SIZEOF_HALF_DMA_BUFFER_IN_BYTES);
    }
}

STATIC void irq_configure(microphone_array_i2s_obj_t *self) {
    if (self->i2s_id == 0) {
        irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
        irq_set_enabled(DMA_IRQ_0, true);
    } else {
        irq_set_exclusive_handler(DMA_IRQ_1, dma_irq1_handler);
        irq_set_enabled(DMA_IRQ_1, true);
    }
}

STATIC void irq_deinit(microphone_array_i2s_obj_t *self) {
    if (self->i2s_id == 0) {
        irq_set_enabled(DMA_IRQ_0, false);
        irq_remove_handler(DMA_IRQ_0, dma_irq0_handler);
    } else {
        irq_set_enabled(DMA_IRQ_1, false);
        irq_remove_handler(DMA_IRQ_1, dma_irq1_handler);
    }
}

STATIC int pio_configure(microphone_array_i2s_obj_t *self) {
    self->pio_program = &microphone_array_i2s_program;

    // find a PIO with a free state machine and adequate program space
    PIO candidate_pio;
    bool is_free_sm;
    bool can_add_program;
    for (uint8_t p = 0; p < NUM_PIOS; p++) {
        candidate_pio = pio_instances[p];
        is_free_sm = false;
        can_add_program = false;

        for (uint8_t sm = 0; sm < NUM_PIO_STATE_MACHINES; sm++) {
            if (!pio_sm_is_claimed(candidate_pio, sm)) {
                is_free_sm = true;
                break;
            }
        }

        if (pio_can_add_program(candidate_pio,  self->pio_program)) {
            can_add_program = true;
        }

        if (is_free_sm && can_add_program) {
            break;
        }
    }

    if (!is_free_sm) {
        return -1;
    }

    if (!can_add_program) {
        return -2;
    }

    self->pio = candidate_pio;
    self->sm = pio_claim_unused_sm(self->pio, false);
    self->prog_offset = pio_add_program(self->pio, self->pio_program);

    // microphone_array_i2s_pio_init(
    //     self->pio, self->sm, self->prog_offset, self->rate,
    //     self->sck, self->ws, 
    //     self->sd + 0, self->sd + 1, 
    //     self->sd + 2, self->sd + 3 );

    pio_sm_init(self->pio, self->sm, self->prog_offset, NULL);

    pio_sm_config config = pio_get_default_sm_config();

    // pio_freq = rate × 128 PIO clock cycles per stereo I2S frame.
    // Each SCK cycle = 2 PIO cycles; fSCK = pio_freq / 2 = rate × 64. ✓
    //
    // On RP2350, set_sys_clock_khz(192000, true) must be called in main()
    // before board_init(). clock_get_hz(clk_sys) then returns 192,000,000,
    // which yields the dividers documented in clocks_audio.h.
    //
    // Use explicit integer+fractional form so the actual register write is
    // visible in the debugger and avoids floating-point rounding surprises.
    // Compute divider with integer rounding-to-nearest on the 16.8 fixed-point
    // divider value to avoid float truncation drift.
    uint32_t pio_freq = (uint32_t)self->rate
        * (uint32_t)SAMPLES_PER_FRAME
        * (uint32_t)dma_get_bits(self->bits)
        * (uint32_t)PIO_INSTRUCTIONS_PER_BIT;
    uint64_t clkdiv_256 = (((uint64_t)clock_get_hz(clk_sys) << 8) + (pio_freq / 2u)) / (uint64_t)pio_freq;
    uint16_t clkdiv_int  = (uint16_t)(clkdiv_256 >> 8);
    uint8_t  clkdiv_frac = (uint8_t)(clkdiv_256 & 0xFFu);
    sm_config_set_clkdiv_int_frac(&config, clkdiv_int, clkdiv_frac);
    self->pio_clkdiv_int = clkdiv_int;
    self->pio_clkdiv_frac = clkdiv_frac;

    
    sm_config_set_in_pins(&config, self->sd_base+0);
    //sm_config_set_in_pins(&config, self->sd_base+1);
    //sm_config_set_in_pins(&config, self->sd_base+2);
    //sm_config_set_in_pins(&config, self->sd_base+3);
    sm_config_set_in_shift(&config, false, true, dma_get_bits(self->bits));
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);  // double RX FIFO size
    

    sm_config_set_sideset(&config, 2, false, false);
    sm_config_set_sideset_pins(&config, self->sck_base);
    sm_config_set_wrap(&config, self->prog_offset, self->prog_offset + self->pio_program->length - 1);
    pio_sm_set_config(self->pio, self->sm, &config);

    return 0;
}

STATIC void pio_deinit(microphone_array_i2s_obj_t *self) {
    if (self->pio) {
        pio_sm_set_enabled(self->pio, self->sm, false);
        pio_sm_unclaim(self->pio, self->sm);
        pio_remove_program(self->pio, self->pio_program, self->prog_offset);
    }
}

STATIC void gpio_init_i2s(PIO pio, uint8_t sm, mp_hal_pin_obj_t pin_num, uint8_t pin_val, gpio_dir_t pin_dir) {
    uint32_t pinmask = 1 << pin_num;
    pio_sm_set_pins_with_mask(pio, sm, pin_val << pin_num, pinmask);
    pio_sm_set_pindirs_with_mask(pio, sm, pin_dir << pin_num, pinmask);
    pio_gpio_init(pio, pin_num);
}

// Configure SD input pads for RP2350 compatibility.
//
// RP2350 difference from RP2040: every pad resets with IE (Input Enable) = 0.
// A PIO state machine reading a data pin whose IE is cleared silently receives
// zeros — no error is raised. gpio_set_dir(GPIO_IN) alone does NOT guarantee
// IE is set on RP2350.
//
// Errata E9 (RP2350-A2): setting PDE (pull-down enable, bit 2) on a pad that
// also has IE = 1 can latch the input into a stuck high-impedance state when
// the pin voltage floats between VIL and VIH. The INMP441 SD pin tri-states
// for ~8 SCK cycles per half-frame (25% duty cycle), which is exactly the
// condition that triggers E9. Never set PDE on an SD input.
//
// Pad register layout (PADS_BANK0_BASE + 0x04 + gpio*4):
//   bit 1 = SCHMITT  (hysteresis)
//   bit 2 = PDE      (pull-down enable) — MUST stay 0 on SD pins
//   bit 3 = PUE      (pull-up enable)   — MUST stay 0 (fights 100kΩ module R)
//   bit 6 = IE       (input enable)     — MUST be 1 before pio_sm_init()
//
// SCK and WS are outputs — do NOT call this function on them.
STATIC void configure_sd_pads(const uint *sd_gpios, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint gpio = sd_gpios[i];
        // GPIO 23, 24, 25 are CYW43-routed on Pico 2W; unusable as PIO pins.
        assert(gpio != 23u && gpio != 24u && gpio != 25u);

        volatile uint32_t *pad =
            (volatile uint32_t *)(PADS_BANK0_BASE + 0x04u + (gpio * 0x04u));

        hw_set_bits(pad, 1u << 6);    // IE = 1: mandatory for PIO input
        hw_set_bits(pad, 1u << 1);    // SCHMITT = 1: hysteresis through 100kΩ RC
        hw_clear_bits(pad, 1u << 2);  // PDE = 0: prevents E9 latch-up
        hw_clear_bits(pad, 1u << 3);  // PUE = 0: no pull-up (fights module pull-down)
    }
}

STATIC void gpio_configure(microphone_array_i2s_obj_t *self) {
    gpio_init_i2s(self->pio, self->sm, self->sck_base+0, 0, GP_OUTPUT);
    gpio_init_i2s(self->pio, self->sm, self->sck_base+1, 0, GP_OUTPUT);
    // Ensure strong drive for clock outputs on longer traces/cables.
    gpio_set_drive_strength(self->sck_base + 0, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(self->sck_base + 1, GPIO_DRIVE_STRENGTH_12MA);

    gpio_init_i2s(self->pio, self->sm, self->sd_base+0, 0, GP_INPUT);
    gpio_init_i2s(self->pio, self->sm, self->sd_base+1, 0, GP_INPUT);
    gpio_init_i2s(self->pio, self->sm, self->sd_base+2, 0, GP_INPUT);
    // SD3 is intentionally unused in the 6-mic layout (only SD0..SD2 carry mics).
    // Keep it as a plain GPIO output driven low so the fourth sampled bit is
    // deterministic zero, rather than a floating input lane.
    gpio_init(self->sd_base+3);
    gpio_set_dir(self->sd_base+3, GPIO_OUT);
    gpio_put(self->sd_base+3, 0);

    // Set IE + SCHMITT, clear PDE + PUE on active SD inputs before pio_sm_init().
    // Must happen after gpio_init_i2s() (which configures pin mux) and before
    // pio_sm_init() (which starts the state machine sampling the pins).
    const uint sd_pins[3] = {
        self->sd_base+0, self->sd_base+1, self->sd_base+2,
    };
    configure_sd_pads(sd_pins, 3);
}

STATIC uint8_t dma_get_bits(int8_t bits) {
    // always read 32 bit words for I2S e.g.  I2S MEMS microphones
    return 32;
}

// determine which DMA channel is associated to this IRQ
STATIC uint dma_map_irq_to_channel(uint irq_index) {
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if ((dma_irqn_get_channel_status(irq_index, ch))) {
            return ch;
        }
    }
    // This should never happen
    return -1;
}

// note:  first DMA channel is mapped to the top half of buffer, second is mapped to the bottom half
STATIC uint8_t *dma_get_buffer(microphone_array_i2s_obj_t *i2s_obj, uint channel) {
    for (uint8_t ch = 0; ch < I2S_NUM_DMA_CHANNELS; ch++) {
        if (i2s_obj->dma_channel[ch] == channel) {
            return i2s_obj->dma_buffer + (SIZEOF_HALF_DMA_BUFFER_IN_BYTES * ch);
        }
    }
    // This should never happen
    return NULL;
}

STATIC int dma_configure(microphone_array_i2s_obj_t *self) {
    // Claim channels dynamically so channel indices are never hardcoded.
    // RP2350 has 16 DMA channels (vs 12 on RP2040); dma_claim_unused_channel
    // handles both counts correctly.
    // Pass false here and validate below, matching original error-return style.
    uint8_t num_free_dma_channels = 0;
    for (uint8_t ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
        if (!dma_channel_is_claimed(ch)) {
            num_free_dma_channels++;
        }
    }
    if (num_free_dma_channels < I2S_NUM_DMA_CHANNELS) {
        return -1;
    }

    for (uint8_t ch = 0; ch < I2S_NUM_DMA_CHANNELS; ch++) {
        self->dma_channel[ch] = dma_claim_unused_channel(false);
    }

    // RP2350: DMA channel state survives a soft reset. Abort any in-flight
    // transfer from a previous firmware run before reconfiguring, otherwise
    // the channel may replay stale data into the ring buffer on first IRQ.
    for (uint8_t ch = 0; ch < I2S_NUM_DMA_CHANNELS; ch++) {
        dma_channel_abort(self->dma_channel[ch]);
        while (dma_channel_is_busy(self->dma_channel[ch])) {
            tight_loop_contents();
        }
    }

    // The DMA channels are chained together.  The first DMA channel is used to access
    // the top half of the DMA buffer.  The second DMA channel accesses the bottom half of the DMA buffer.
    // With chaining, when one DMA channel has completed a data transfer, the other
    // DMA channel automatically starts a new data transfer.
    enum dma_channel_transfer_size dma_size = (dma_get_bits(self->bits) == 16) ? DMA_SIZE_16 : DMA_SIZE_32;
    for (uint8_t ch = 0; ch < I2S_NUM_DMA_CHANNELS; ch++) {
        dma_channel_config dma_config = dma_channel_get_default_config(self->dma_channel[ch]);
        channel_config_set_transfer_data_size(&dma_config, dma_size);
        channel_config_set_chain_to(&dma_config, self->dma_channel[(ch + 1) % I2S_NUM_DMA_CHANNELS]);

        uint8_t *dma_buffer = self->dma_buffer + (SIZEOF_HALF_DMA_BUFFER_IN_BYTES * ch);
        
        channel_config_set_dreq(&dma_config, pio_get_dreq(self->pio, self->sm, false));
        channel_config_set_read_increment(&dma_config, false);
        channel_config_set_write_increment(&dma_config, true);
        dma_channel_configure(self->dma_channel[ch],
            &dma_config,
            dma_buffer,                                             // dest = DMA buffer
            (void *)&self->pio->rxf[self->sm],                      // src = PIO RX FIFO
            SIZEOF_HALF_DMA_BUFFER_IN_BYTES / (dma_get_bits(self->bits) / 8),
            false);
        
    }

    for (uint8_t ch = 0; ch < I2S_NUM_DMA_CHANNELS; ch++) {
        dma_irqn_acknowledge_channel(self->i2s_id, self->dma_channel[ch]);  // clear pending.  e.g. from SPI
        dma_irqn_set_channel_enabled(self->i2s_id, self->dma_channel[ch], true);
    }

    return 0;
}

STATIC void dma_deinit(microphone_array_i2s_obj_t *self) {
    for (uint8_t ch = 0; ch < I2S_NUM_DMA_CHANNELS; ch++) {
        int channel = self->dma_channel[ch];

        // unchain the channel to prevent triggering a transfer in the chained-to channel
        dma_channel_config dma_config = dma_get_channel_config(channel);
        channel_config_set_chain_to(&dma_config, channel);
        dma_channel_set_config(channel, &dma_config, false);

        dma_irqn_set_channel_enabled(self->i2s_id, channel, false);
        dma_channel_abort(channel);  // in case a transfer is in flight
        dma_channel_unclaim(channel);
    }
}

STATIC void dma_irq_handler(uint8_t irq_index) {
    _i2s_dma_irq_count++;

    int dma_channel = dma_map_irq_to_channel(irq_index);
    if (dma_channel == -1) {
        // This should never happen
        return;
    }

    microphone_array_i2s_obj_t *self = microphone_array_i2s_obj[irq_index];
    if (self == NULL) {
        // This should never happen
        return;
    }

    uint8_t *dma_buffer = dma_get_buffer(self, dma_channel);
    if (dma_buffer == NULL) {
        // This should never happen
        return;
    }

    empty_dma(self, dma_buffer);
    dma_irqn_acknowledge_channel(irq_index, dma_channel);
    dma_channel_set_write_addr(dma_channel, dma_buffer, false);
}

STATIC void dma_irq0_handler(void) {
    dma_irq_handler(0);
}

STATIC void dma_irq1_handler(void) {
    dma_irq_handler(1);
}


STATIC int microphone_array_i2s_init_helper(microphone_array_i2s_obj_t *self,
              mp_hal_pin_obj_t sck_base, mp_hal_pin_obj_t sd_base,
              int8_t i2s_bits, int32_t ring_buffer_len, int32_t i2s_rate) {
    //
    // ---- Check validity of arguments ----
    //

    // is Bits valid?
    if ((i2s_bits != 16) &&
        (i2s_bits != 32)) {
        _last_i2s_init_error = -3;
        return -3;
    }

    // is Rate valid?
    // Not checked

    // is Ibuf valid?
    if (ring_buffer_len > 0) {
        self->ring_buffer_storage = m_new(uint8_t, ring_buffer_len);
        if (self->ring_buffer_storage == NULL) {
            _last_i2s_init_error = -6;
            return -6;
        }
        ringbuf_init(&self->ring_buffer, self->ring_buffer_storage, ring_buffer_len);
    } else {
        _last_i2s_init_error = -5;
        return -5;
    }

    self->sck_base = sck_base;
    self->sd_base = sd_base;
    self->bits = i2s_bits;
    self->rate = i2s_rate;
    self->ibuf = ring_buffer_len;
    self->io_mode = BLOCKING;

    irq_configure(self);
    int err = pio_configure(self);
    if (err != 0) {
        _last_i2s_init_error = (int8_t)err;
        return err;
    }
    gpio_configure(self);
    err = dma_configure(self);
    if (err != 0) {
        _last_i2s_init_error = (int8_t)(err - 10); // -11 = no free DMA channels
        return err;
    }
    _last_i2s_init_error = 0;

    pio_sm_set_enabled(self->pio, self->sm, true);
    dma_channel_start(self->dma_channel[0]);

    return 0;
}

STATIC microphone_array_i2s_obj_t* microphone_array_i2s_make_new(uint8_t i2s_id,
              mp_hal_pin_obj_t sck_base, mp_hal_pin_obj_t sd_base,
              int8_t i2s_bits, int32_t ring_buffer_len, int32_t i2s_rate) {
    if (i2s_id >= MAX_I2S_RP2) {
        return NULL;
    }

    microphone_array_i2s_obj_t *self;
    if (microphone_array_i2s_obj[i2s_id] == NULL) {
        self = m_new_obj(microphone_array_i2s_obj_t);
        microphone_array_i2s_obj[i2s_id] = self;
        self->i2s_id = i2s_id;
    } else {
        self = microphone_array_i2s_obj[i2s_id];
        microphone_array_i2s_deinit(self);
    }

    if (microphone_array_i2s_init_helper(self, sck_base, sd_base, i2s_bits,
            ring_buffer_len, i2s_rate) != 0) {
        return NULL;
    }
    return self;
}

STATIC void microphone_array_i2s_deinit(microphone_array_i2s_obj_t *self) {
    // use self->pio as in indication that I2S object has already been de-initialized
    if (self->pio != NULL) {
        pio_deinit(self);
        dma_deinit(self);
        irq_deinit(self);
        free(self->ring_buffer_storage);
        self->pio = NULL;  // flag object as de-initialized
    }
}

STATIC int microphone_array_i2s_stream_read(microphone_array_i2s_obj_t *self, void *buf_in, size_t size) {
    uint8_t appbuf_sample_size_in_bytes = (self->bits / 8) * 2;
    if (size % appbuf_sample_size_in_bytes != 0) {
        return -2;
    }

    if (size == 0) {
        return 0;
    }

    mp_buffer_info_t appbuf;
    appbuf.buf = (void *)buf_in;
    appbuf.len = size;
    uint32_t num_bytes_read = fill_appbuf_from_ringbuf(self, &appbuf);
    return num_bytes_read;
}


microphone_array_i2s_obj_t* create_microphone_array_i2s(uint8_t i2s_id,
              mp_hal_pin_obj_t sck_base, mp_hal_pin_obj_t sd_base,
              int8_t i2s_bits, int32_t ring_buffer_len, int32_t i2s_rate)
{
    return microphone_array_i2s_make_new(i2s_id, sck_base, sd_base, i2s_bits, ring_buffer_len, i2s_rate);
}


int microphone_array_i2s_read_stream(microphone_array_i2s_obj_t *self, void *buf_in, size_t size)
{
    return microphone_array_i2s_stream_read(self, buf_in, size);
}

int microphone_array_i2s_read_stream_nonblocking(microphone_array_i2s_obj_t *self, void *buf_in, size_t size)
{
    uint8_t appbuf_sample_size_in_bytes = (self->bits / 8) * 2;
    if (size % appbuf_sample_size_in_bytes != 0) {
        return -2;
    }

    if (size == 0) {
        return 0;
    }

    size_t available = ringbuf_available_data(&self->ring_buffer);
    size_t bytes_to_copy = (available < size) ? available : size;
    bytes_to_copy -= (bytes_to_copy % appbuf_sample_size_in_bytes);

    uint8_t *dst = (uint8_t *)buf_in;
    size_t copied = 0;
    while (copied < bytes_to_copy) {
        if (!ringbuf_pop(&self->ring_buffer, dst + copied)) {
            break;
        }
        copied++;
    }

    return (int)copied;
}


i2s_audio_sample decode_sample(i2s_audio_sample* sample_ptr_in)
{
    i2s_audio_sample res;

    for(int sample_idx = 0; sample_idx < 4; sample_idx++)
    {
        res.sample_l[sample_idx] = 0;
        res.sample_r[sample_idx] = 0;
    }

    for(int sample_idx = 0; sample_idx < 4; sample_idx++)
    {
        for(int slice_idx=7; slice_idx >=0; slice_idx--){
            uint32_t bit_slice_4 = (sample_ptr_in->sample_l[sample_idx]>>(slice_idx*4));
            bool bit_sample_0 = (bit_slice_4 & 0x1) != 0x0;
            bool bit_sample_1 = (bit_slice_4 & 0x2) != 0x0;
            bool bit_sample_2 = (bit_slice_4 & 0x4) != 0x0;
            bool bit_sample_3 = (bit_slice_4 & 0x8) != 0x0;

            res.sample_l[0] = (res.sample_l[0]<<1) | bit_sample_0;
            res.sample_l[1] = (res.sample_l[1]<<1) | bit_sample_1;
            res.sample_l[2] = (res.sample_l[2]<<1) | bit_sample_2;
            res.sample_l[3] = (res.sample_l[3]<<1) | bit_sample_3;
        }
    }

    for(int sample_idx = 0; sample_idx < 4; sample_idx++)
    {
        for(int slice_idx=7; slice_idx >=0; slice_idx--){
            uint32_t bit_slice_4 = (sample_ptr_in->sample_r[sample_idx]>>(slice_idx*4));
            bool bit_sample_0 = (bit_slice_4 & 0x1) != 0x0;
            bool bit_sample_1 = (bit_slice_4 & 0x2) != 0x0;
            bool bit_sample_2 = (bit_slice_4 & 0x4) != 0x0;
            bool bit_sample_3 = (bit_slice_4 & 0x8) != 0x0;

            res.sample_r[0] = (res.sample_r[0]<<1) | bit_sample_0;
            res.sample_r[1] = (res.sample_r[1]<<1) | bit_sample_1;
            res.sample_r[2] = (res.sample_r[2]<<1) | bit_sample_2;
            res.sample_r[3] = (res.sample_r[3]<<1) | bit_sample_3;
        }
    }

    // printf("DBG IN %.8x %.8x %.8x %.8x %.8x %.8x %.8x %.8x\n",
    //             sample_ptr_in->sample_l[0], sample_ptr_in->sample_l[1], sample_ptr_in->sample_l[2], sample_ptr_in->sample_l[3],
    //             sample_ptr_in->sample_r[0], sample_ptr_in->sample_r[1], sample_ptr_in->sample_r[2], sample_ptr_in->sample_r[3]);

    // printf("DBG OUT %.8x %.8x %.8x %.8x %.8x %.8x %.8x %.8x\n",
    //             res.sample_l[0], res.sample_l[1], res.sample_l[2], res.sample_l[3],
    //             res.sample_r[0], res.sample_r[1], res.sample_r[2], res.sample_r[3]);

    return res;
}