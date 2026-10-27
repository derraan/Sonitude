#include "pcm5101a_i2s.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "pcm5101a_clock.h"
#include "pcm5101a_fifo.h"
#include "pcm5101a_i2s.pio.h"

#define WORDS_PER_BLOCK (2u * PCM5101A_BLOCK_FRAMES)

// Datasheet: PLL starts after 16 LRCK with SCK at ground; allow lock margin.
#define PLL_LOCK_WAIT_US 10000u

static int32_t fifo_storage[2u * PCM5101A_FIFO_FRAMES];
static int32_t __attribute__((aligned(4))) dma_buf[2][WORDS_PER_BLOCK];

static struct {
    pcm5101a_config_t cfg;
    pcm5101a_fifo_t fifo;
    uint sm;
    uint offset;
    int dma_ch[2];
    bool initialized;
    bool running;
    volatile pcm5101a_stats_t stats;
} drv;

static void __not_in_flash_func(refill)(uint idx) {
    int32_t *buf = dma_buf[idx];
    if (!pcm5101a_fifo_pop_exact(&drv.fifo, buf, PCM5101A_BLOCK_FRAMES)) {
        memset(buf, 0, sizeof(dma_buf[idx]));
        drv.stats.underruns++;
    }
    dma_channel_set_read_addr((uint)drv.dma_ch[idx], buf, false);
}

static void __not_in_flash_func(dma_irq_handler)(void) {
    uint32_t level = pcm5101a_fifo_level(&drv.fifo);
    for (uint i = 0; i < 2; i++) {
        uint ch = (uint)drv.dma_ch[i];
        if (dma_irqn_get_channel_status(PCM5101A_DMA_IRQ_INDEX, ch)) {
            dma_irqn_acknowledge_channel(PCM5101A_DMA_IRQ_INDEX, ch);
            if (level > drv.stats.max_fill) {
                drv.stats.max_fill = level;
            }
            refill(i);
            drv.stats.irq_count++;
        }
    }
}

static void configure_dma(uint idx) {
    uint ch = (uint)drv.dma_ch[idx];
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(drv.cfg.pio, drv.sm, true));
    channel_config_set_chain_to(&c, (uint)drv.dma_ch[idx ^ 1u]);
    dma_channel_configure(ch, &c, &drv.cfg.pio->txf[drv.sm], dma_buf[idx], WORDS_PER_BLOCK, false);
    dma_irqn_set_channel_enabled(PCM5101A_DMA_IRQ_INDEX, ch, true);
}

bool pcm5101a_init(const pcm5101a_config_t *cfg) {
    if (drv.initialized || cfg == NULL || cfg->pin_din == cfg->pin_bck || cfg->pin_din == cfg->pin_bck + 1u) {
        return false;
    }

    pcm5101a_clkdiv_t div;
    if (!pcm5101a_compute_clkdiv(clock_get_hz(clk_sys), cfg->sample_rate, &div)) {
        return false;
    }

    int sm = pio_claim_unused_sm(cfg->pio, false);
    if (sm < 0) {
        return false;
    }
    if (!pio_can_add_program(cfg->pio, &pcm5101a_i2s_program)) {
        pio_sm_unclaim(cfg->pio, (uint)sm);
        return false;
    }
    int ch0 = dma_claim_unused_channel(false);
    int ch1 = dma_claim_unused_channel(false);
    if (ch0 < 0 || ch1 < 0) {
        if (ch0 >= 0) dma_channel_unclaim((uint)ch0);
        if (ch1 >= 0) dma_channel_unclaim((uint)ch1);
        pio_sm_unclaim(cfg->pio, (uint)sm);
        return false;
    }

    memset(&drv, 0, sizeof(drv));
    drv.cfg = *cfg;
    drv.sm = (uint)sm;
    drv.dma_ch[0] = ch0;
    drv.dma_ch[1] = ch1;
    drv.stats.clk_error_ppb = div.error_ppb;
    pcm5101a_fifo_init(&drv.fifo, fifo_storage, PCM5101A_FIFO_FRAMES);
    memset(dma_buf, 0, sizeof(dma_buf));

    if (cfg->pin_xsmt >= 0) {
        gpio_init((uint)cfg->pin_xsmt);
        gpio_put((uint)cfg->pin_xsmt, 0);
        gpio_set_dir((uint)cfg->pin_xsmt, GPIO_OUT);
        gpio_set_drive_strength((uint)cfg->pin_xsmt, GPIO_DRIVE_STRENGTH_8MA);
        gpio_set_slew_rate((uint)cfg->pin_xsmt, GPIO_SLEW_RATE_FAST);
    }
    if (cfg->pin_flt >= 0) {
        gpio_init((uint)cfg->pin_flt);
        gpio_put((uint)cfg->pin_flt, 1);
        gpio_set_dir((uint)cfg->pin_flt, GPIO_OUT);
    }

    PIO pio = cfg->pio;
    drv.offset = pio_add_program(pio, &pcm5101a_i2s_program);
    pio_gpio_init(pio, cfg->pin_din);
    pio_gpio_init(pio, cfg->pin_bck);
    pio_gpio_init(pio, cfg->pin_bck + 1u);

    pio_sm_config c = pcm5101a_i2s_program_get_default_config(drv.offset);
    sm_config_set_out_pins(&c, cfg->pin_din, 1);
    sm_config_set_sideset_pins(&c, cfg->pin_bck);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv_int_frac(&c, div.div_int, div.div_frac);
    pio_sm_init(pio, drv.sm, drv.offset + pcm5101a_i2s_offset_entry_point, &c);

    uint32_t pin_mask = (1u << cfg->pin_din) | (3u << cfg->pin_bck);
    pio_sm_set_pins_with_mask(pio, drv.sm, 0, pin_mask);
    pio_sm_set_pindirs_with_mask(pio, drv.sm, pin_mask, pin_mask);

    configure_dma(0);
    configure_dma(1);

    uint irq_num = dma_get_irq_num(PCM5101A_DMA_IRQ_INDEX);
    irq_add_shared_handler(irq_num, dma_irq_handler, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
    irq_set_priority(irq_num, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(irq_num, true);

    drv.initialized = true;
    return true;
}

void pcm5101a_start(void) {
    if (!drv.initialized || drv.running) {
        return;
    }
    pcm5101a_set_mute(true);
    memset(dma_buf, 0, sizeof(dma_buf));
    dma_channel_set_read_addr((uint)drv.dma_ch[0], dma_buf[0], false);
    dma_channel_set_read_addr((uint)drv.dma_ch[1], dma_buf[1], false);
    dma_channel_set_trans_count((uint)drv.dma_ch[0], WORDS_PER_BLOCK, false);
    dma_channel_set_trans_count((uint)drv.dma_ch[1], WORDS_PER_BLOCK, false);

    // Fill the PIO FIFO before enabling the SM so the first BCK edges are clean.
    dma_channel_start((uint)drv.dma_ch[0]);
    while (!pio_sm_is_tx_fifo_full(drv.cfg.pio, drv.sm)) {
        tight_loop_contents();
    }
    pio_sm_set_enabled(drv.cfg.pio, drv.sm, true);
    drv.running = true;

    sleep_us(PLL_LOCK_WAIT_US);
    pcm5101a_set_mute(false);
}

void pcm5101a_stop(void) {
    if (!drv.running) {
        return;
    }
    pcm5101a_set_mute(true);
    // Datasheet: soft mute + analog mute completes within 150 tS + 0.2 ms.
    sleep_us(150u * 1000000u / drv.cfg.sample_rate + 200u + 500u);

    uint32_t mask = (1u << (uint)drv.dma_ch[0]) | (1u << (uint)drv.dma_ch[1]);
    dma_irqn_set_channel_mask_enabled(PCM5101A_DMA_IRQ_INDEX, mask, false);
    // Break the chain before aborting so the partner cannot re-trigger.
    hw_clear_bits(&dma_hw->ch[drv.dma_ch[0]].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[drv.dma_ch[1]].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->abort = mask;
    while (dma_hw->abort & mask) {
        tight_loop_contents();
    }
    hw_set_bits(&dma_hw->ch[drv.dma_ch[0]].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_set_bits(&dma_hw->ch[drv.dma_ch[1]].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_irqn_acknowledge_channel(PCM5101A_DMA_IRQ_INDEX, (uint)drv.dma_ch[0]);
    dma_irqn_acknowledge_channel(PCM5101A_DMA_IRQ_INDEX, (uint)drv.dma_ch[1]);
    dma_irqn_set_channel_mask_enabled(PCM5101A_DMA_IRQ_INDEX, mask, true);

    PIO pio = drv.cfg.pio;
    pio_sm_set_enabled(pio, drv.sm, false);
    pio_sm_clear_fifos(pio, drv.sm);
    pio_sm_restart(pio, drv.sm);
    pio_sm_exec(pio, drv.sm, pio_encode_jmp(drv.offset + pcm5101a_i2s_offset_entry_point));
    pio_sm_set_pins_with_mask(pio, drv.sm, 0, (1u << drv.cfg.pin_din) | (3u << drv.cfg.pin_bck));
    drv.running = false;
}

size_t pcm5101a_write(const int32_t *lr, size_t frames) {
    size_t n = pcm5101a_fifo_push(&drv.fifo, lr, frames);
    if (n < frames) {
        drv.stats.overflows++;
        drv.stats.dropped_frames += (uint32_t)(frames - n);
    }
    return n;
}

size_t pcm5101a_fill_level(void) {
    return pcm5101a_fifo_level(&drv.fifo);
}

void pcm5101a_set_mute(bool mute) {
    if (drv.cfg.pin_xsmt >= 0) {
        gpio_put((uint)drv.cfg.pin_xsmt, !mute);
    }
}

void pcm5101a_get_stats(pcm5101a_stats_t *out) {
    uint32_t save = save_and_disable_interrupts();
    memcpy(out, (const void *)&drv.stats, sizeof(*out));
    restore_interrupts(save);
}
