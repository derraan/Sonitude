/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Reinhard Panhuber
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

/* plot_audio_samples.py requires following modules:
 * $ sudo apt install libportaudio
 * $ pip3 install sounddevice matplotlib
 *
 * Then run
 * $ python3 plot_audio_samples.py
 */

#ifndef USB_AUDIO_DEBUG_STAGE
#define USB_AUDIO_DEBUG_STAGE 5
#endif

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico/clocks_audio.h"   // AUDIO_F_SYS_KHZ and PIO divider constants

#if USB_AUDIO_DEBUG_STAGE != 0
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tusb_config.h"
#include "main.h"
#if USB_AUDIO_DEBUG_STAGE != 3
#include "microphone_settings.h"
#include "pico/microphone_array_i2s.h"
#include "pico/volume_ctrl.h"
#endif

#include "bsp/board_api.h"
#include "tusb.h"
#endif

// GPIO 25 is routed through the CYW43 chip on Pico 2W — unusable as a direct
// GPIO output. Use GPIO 22, which is a free general-purpose pin on Pico 2W.
// led_blinking_task() continues to use board_led_write() (BSP-managed CYW43).
#define DEBUG_LED_PIN 22

static void debug_led_init_once(void) {
  static bool initialized = false;
  if (initialized) {
    return;
  }

  gpio_init(DEBUG_LED_PIN);
  gpio_set_dir(DEBUG_LED_PIN, GPIO_OUT);
  gpio_put(DEBUG_LED_PIN, 0);
  initialized = true;
}

static void debug_led_set(bool state) {
  debug_led_init_once();
  gpio_put(DEBUG_LED_PIN, state ? 1 : 0);
}

static void debug_led_pulse_blocking(uint32_t on_ms, uint32_t off_ms) {
  debug_led_set(true);
  sleep_ms(on_ms);
  debug_led_set(false);
  sleep_ms(off_ms);
}

static void debug_stage1_boot_probe(void) {
#if USB_AUDIO_DEBUG_STAGE >= 1
  debug_led_init_once();
  debug_led_pulse_blocking(150, 150);
  debug_led_pulse_blocking(150, 150);
  debug_led_pulse_blocking(150, 150);
#endif
}

static void debug_stage2_post_tusb_probe(void) {
#if USB_AUDIO_DEBUG_STAGE >= 2
  debug_led_pulse_blocking(200, 150);
#endif
}

static void debug_stage2_loop_probe(void) {
#if USB_AUDIO_DEBUG_STAGE >= 2
  static uint32_t last_transition_ms = 0;
  static bool pulse_active = false;
  uint32_t now = board_millis();

  if (!pulse_active && (now - last_transition_ms) >= 500) {
    debug_led_set(true);
    last_transition_ms = now;
    pulse_active = true;
  } else if (pulse_active && (now - last_transition_ms) >= 20) {
    debug_led_set(false);
    last_transition_ms = now;
    pulse_active = false;
  }
#endif
}

#if USB_AUDIO_DEBUG_STAGE == 0
int main(void) {
  // 192 MHz: exact integer PLL config (FBDIV=64, POSTDIV1=2, POSTDIV2=2,
  // VCO=768 MHz). USB PLL is independent — USB is unaffected.
  set_sys_clock_khz(AUDIO_F_SYS_KHZ, true);
  stdio_init_all();
  debug_led_init_once();

  while (true) {
    debug_led_pulse_blocking(100, 900);
    printf("usb_audio_stage0 alive\n");
  }
}
#elif USB_AUDIO_DEBUG_STAGE != 3

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+
#define AUDIO_SAMPLE_RATE CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE
// Buffer sizing must cover the highest runtime-selectable rate (48 kHz = 49 samples/ms)
// regardless of the compile-time default rate. Using AUDIO_SAMPLE_RATE here would
// produce a buffer too small for 48 kHz if the default is 24 or 44.1 kHz, causing
// silent overflow when the host selects a higher rate at runtime.
#define AUDIO_SAMPLE_RATE_MAX 48000u
// Ring buffer anchored to worst-case runtime rate, mirroring USB buffer sizing policy.
// At 48 kHz, 8 channels, 32-bit samples: 1 ms payload = 1536 bytes.
// 32x gives 49152 bytes total, i.e. ~32 ms headroom at 48 kHz.
#define SIZEOF_I2S_RING_BUFFER_IN_BYTES \
  (((AUDIO_SAMPLE_RATE_MAX * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX * (uint32_t)sizeof(int32_t)) / 1000u) * 32u)
#define USB_FRAME_SAMPLES_MAX ((AUDIO_SAMPLE_RATE_MAX + 999u) / 1000u)
// Per-FIFO row: enough uint32_t slots for max channels x max ms samples, split across FIFOs (RP2040 original layout).
#define I2S_USB_SAMPLES_PER_FIFO_ROW \
  ((CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX * USB_FRAME_SAMPLES_MAX + CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO - 1) / CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO)
#define UNUSED_SD3_PIN (PIN_SD2 + 1)

#if USB_AUDIO_DEBUG_STAGE == 4 || USB_AUDIO_DEBUG_STAGE == 7
typedef uint32_t usb_tx_sample_t;
#else
typedef int16_t usb_tx_sample_t;
#endif

/* Blink pattern
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum {
  BLINK_NOT_MOUNTED = 250,
  BLINK_MOUNTED = 1000,
  BLINK_SUSPENDED = 2500,
};

microphone_settings_t microphone_settings;

// Audio controls
// Current states
bool mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];      // +1 for master channel 0
uint16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];// +1 for master channel 0
uint32_t sampFreq;
uint8_t clkValid;
static uint32_t usb_frame_accumulator = 0;

// Range states
audio_control_range_2_n_t(1) volumeRng[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];// Volume range state
audio_control_range_4_n_t(4) sampleFreqRng;                                    // Sample frequency range state
static volatile uint32_t pendingSampFreq = 0;

#if CFG_TUD_AUDIO_ENABLE_ENCODING
usb_tx_sample_t i2s_dummy_buffer[CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO][I2S_USB_SAMPLES_PER_FIFO_ROW];
#else
usb_tx_sample_t i2s_dummy_buffer[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX * USB_FRAME_SAMPLES_MAX];
#endif

void led_blinking_task(void);
void audio_task(void);

//-------------------------
// callback functions
//-------------------------
void usb_microphone_mute_handler(int8_t bChannelNumber, int8_t mute_in);
void usb_microphone_volume_handler(int8_t bChannelNumber, int16_t volume_in);
void usb_microphone_current_sample_rate_handler(uint32_t current_sample_rate_in);
void usb_microphone_current_resolution_handler(uint8_t current_resolution_in);
void usb_microphone_current_status_set_handler(uint32_t blink_interval_ms_in);
void on_usb_microphone_tx_pre_load(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting);
void on_usb_microphone_tx_post_load(uint8_t rhport, uint16_t n_bytes_copied, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting);
usb_tx_sample_t i2s_to_usb_sample_convert(uint32_t sample, uint32_t volume_db);
uint16_t get_usb_frame_samples(void);
static void usb_audio_post_load_fill(void);
static bool is_supported_sample_rate(uint32_t rate);
#if USB_AUDIO_DEBUG_STAGE == 6
void debug_stage6_cdc_levels_task(void);
#endif
//-------------------------

// Pointer to I2S handler
microphone_array_i2s_obj_t* i2s0 = NULL;
static uint16_t usb_tx_frame_samples = 0;
// Last good decoded sample per logical channel (6 active channels).
// Used to fill short I2S reads with continuity rather than silence,
// avoiding abrupt zero-edges that produce ring-mod/AM artifacts.
static usb_tx_sample_t last_good_sample[6] = {0};
static volatile uint8_t audio_stream_alt_setting = 0;
// Pre-fill gate: counts down from RING_PREFILL_CALLBACKS after alt=1.
// While > 0 the USB fill path is held (ring accumulates); countdown resumes
// each callback so DMA has ~20 ms headroom before first USB read.
// This absorbs Windows USB host scheduling stalls (~5-15 ms DPC latency spikes)
// that would otherwise immediately starve the newly-started ring.
#define RING_PREFILL_CALLBACKS 20u
static volatile uint8_t ring_prefill_gate = 0u;
// Soft re-entry crossfade: after total starvation, blend the first
// RECOVERY_BLEND_LEN frames from the decayed last_good_sample value back
// to live audio. This eliminates the abrupt amplitude jump at recovery
// that is the primary source of audible ring-mod transients.
// RECOVERY_BLEND_LEN must be a power of 2 for the >> shift to work.
#define RECOVERY_BLEND_LEN 32u
static uint8_t recovery_blend_remaining = 0u;
#if USB_AUDIO_DEBUG_STAGE >= 4
// UAC2 streaming probes (debugger or Stage 6 CDC)
static volatile uint32_t debug_s6_prepare_called = 0;
static volatile uint8_t  debug_s6_i2s_null = 0;
static volatile uint32_t debug_s6_alt_set_count = 0;
static volatile int8_t   debug_s6_cur_alt = -1;
static volatile uint32_t debug_s6_tx_pre_count = 0;
static volatile uint32_t debug_s6_tx_post_count = 0;
#endif
#if USB_AUDIO_DEBUG_STAGE == 6
static volatile uint16_t debug_stage6_peak[6] = {0};
static volatile int debug_stage6_last_num_bytes_read = 0;
static volatile uint16_t debug_stage6_last_frames_to_copy = 0;
#endif

static bool is_supported_sample_rate(uint32_t rate) {
  switch (rate) {
    case 16000u:
    case 24000u:
    case 44100u:
    case 48000u:
      return true;
    default:
      return false;
  }
}

void refresh_i2s_connections()
{
  // microphone_settings.samples_in_i2s_frame_min = (microphone_settings.sample_rate)    /1000;
  // microphone_settings.samples_in_i2s_frame_max = (microphone_settings.sample_rate+999)/1000;

  i2s0 = create_microphone_array_i2s(
      0,
      PIN_SCK,
      PIN_SD0,
      I2S_BPS,
      SIZEOF_I2S_RING_BUFFER_IN_BYTES,
      (int32_t)microphone_settings.sample_rate);
}

/*------------- MAIN -------------*/
int main(void) {
  // Must be the very first call — before board_init() and any peripheral init.
  // USB PLL (48 MHz USBPLL) is independent; USB enumeration is unaffected.
  // 192 MHz: FBDIV=64, POSTDIV1=2, POSTDIV2=2, VCO=768 MHz (in-spec RP2350).
  set_sys_clock_khz(AUDIO_F_SYS_KHZ, true);

  debug_stage1_boot_probe();

  stdio_init_all();

  board_init();

  // Drive the unused fourth SD lane low before the PIO state machine starts.
  // RP2350 Errata E9: GPIO_IN + gpio_pull_down() sets IE=1 and PDE=1 together.
  // When the floating pin voltage drifts between VIL and VIH during the
  // INMP441 SD tri-state window, this combination latches the input into a
  // stuck hi-Z state. Drive as GPIO_OUT LOW instead — no pull resistor needed.
  gpio_init(UNUSED_SD3_PIN);
  gpio_set_dir(UNUSED_SD3_PIN, GPIO_OUT);
  gpio_put(UNUSED_SD3_PIN, 0);

  tud_init(BOARD_TUD_RHPORT);

  if (board_init_after_tusb) {
    board_init_after_tusb();
  }
  debug_stage2_post_tusb_probe();

  // Init values
  sampFreq = AUDIO_SAMPLE_RATE;
  clkValid = 1;
  microphone_settings.sample_rate = sampFreq;
  sampleFreqRng.wNumSubRanges = 4;
  sampleFreqRng.subrange[0].bMin = 16000;
  sampleFreqRng.subrange[0].bMax = 16000;
  sampleFreqRng.subrange[0].bRes = 0;
  sampleFreqRng.subrange[1].bMin = 24000;
  sampleFreqRng.subrange[1].bMax = 24000;
  sampleFreqRng.subrange[1].bRes = 0;
  sampleFreqRng.subrange[2].bMin = 44100;
  sampleFreqRng.subrange[2].bMax = 44100;
  sampleFreqRng.subrange[2].bRes = 0;
  sampleFreqRng.subrange[3].bMin = 48000;
  sampleFreqRng.subrange[3].bMax = 48000;
  sampleFreqRng.subrange[3].bRes = 0;

#if USB_AUDIO_DEBUG_STAGE >= 4
  refresh_i2s_connections();
#endif


  while (1) {
    tud_task();// tinyusb device task
    debug_stage2_loop_probe();
    led_blinking_task();
    audio_task();
#if USB_AUDIO_DEBUG_STAGE == 6
    debug_stage6_cdc_levels_task();
#endif
  }
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void) {
  usb_microphone_current_status_set_handler(BLINK_MOUNTED);
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
  usb_microphone_current_status_set_handler(BLINK_NOT_MOUNTED);
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
  usb_microphone_current_status_set_handler(BLINK_SUSPENDED);
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
  usb_microphone_current_status_set_handler(tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED);
}

//--------------------------------------------------------------------+
// AUDIO Task
//--------------------------------------------------------------------+

// This task simulates an audio receive callback, one frame is received every 1ms.
// We assume that the audio data is read from an I2S buffer.
// In a real application, this would be replaced with actual I2S receive callback.
void audio_task(void) {
}

//--------------------------------------------------------------------+
// Application Callback API Implementations
//--------------------------------------------------------------------+

// Invoked when audio class specific set request received for an EP
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void) rhport;
  (void) pBuff;

  // We do not support any set range requests here, only current value requests
  TU_VERIFY(p_request->bRequest == AUDIO_CS_REQ_CUR);

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t ep = TU_U16_LOW(p_request->wIndex);

  (void) channelNum;
  (void) ctrlSel;
  (void) ep;

  return false;// Yet not implemented
}

// Invoked when audio class specific set request received for an interface
bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void) rhport;
  (void) pBuff;

  // We do not support any set range requests here, only current value requests
  TU_VERIFY(p_request->bRequest == AUDIO_CS_REQ_CUR);

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t itf = TU_U16_LOW(p_request->wIndex);

  (void) channelNum;
  (void) ctrlSel;
  (void) itf;

  return false;// Yet not implemented
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t itf = TU_U16_LOW(p_request->wIndex);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  (void) itf;

  // We do not support any set range requests here, only current value requests
  TU_VERIFY(p_request->bRequest == AUDIO_CS_REQ_CUR);

  // If request is for our feature unit
  if (entityID == 2) {
    switch (ctrlSel) {
      case AUDIO_FU_CTRL_MUTE:
        // Request uses format layout 1
        TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_1_t));

        mute[channelNum] = ((audio_control_cur_1_t *) pBuff)->bCur;
        usb_microphone_mute_handler(channelNum, mute[channelNum]);

        TU_LOG2("    Set Mute: %d of channel: %u\r\n", mute[channelNum], channelNum);
        return true;

      case AUDIO_FU_CTRL_VOLUME:
        // Request uses format layout 2
        TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_2_t));

        volume[channelNum] = (uint16_t) ((audio_control_cur_2_t *) pBuff)->bCur;
        usb_microphone_volume_handler(channelNum, volume[channelNum]);

        TU_LOG2("    Set Volume: %d dB of channel: %u\r\n", volume[channelNum], channelNum);
        return true;

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  // Clock Source unit
  if (entityID == 4) {
    switch (ctrlSel) {
      case AUDIO_CS_CTRL_SAM_FREQ:
        // Runtime-selectable sample-rate contract:
        // - accept host-requested rates from supported discrete list
        // - apply immediately when idle (alt=0)
        // - defer while streaming (alt=1) and apply at next stop
        TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_4_t));
        {
          uint32_t req_rate = (uint32_t)((audio_control_cur_4_t *)pBuff)->bCur;
          if (!is_supported_sample_rate(req_rate)) {
            TU_LOG2("    Reject unsupported Sample Freq %lu\r\n", (unsigned long)req_rate);
            return false;
          }

          if (audio_stream_alt_setting == 0u) {
            sampFreq = req_rate;
            usb_microphone_current_sample_rate_handler(req_rate);
            pendingSampFreq = 0;
            TU_LOG2("    Apply Sample Freq now: %lu\r\n", (unsigned long)req_rate);
            return true;
          }

          pendingSampFreq = req_rate;
          TU_LOG2("    Defer Sample Freq %lu until stream stop\r\n", (unsigned long)req_rate);
          return true;
        }

      default:
        TU_BREAKPOINT();
        return false;
    }
  }
  return false;// Yet not implemented
}

// Invoked when audio class specific get request received for an EP
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t ep = TU_U16_LOW(p_request->wIndex);

  (void) channelNum;
  (void) ctrlSel;
  (void) ep;

  //  return tud_control_xfer(rhport, p_request, &tmp, 1);

  return false;// Yet not implemented
}

// Invoked when audio class specific get request received for an interface
bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t itf = TU_U16_LOW(p_request->wIndex);

  (void) channelNum;
  (void) ctrlSel;
  (void) itf;

  return false;// Yet not implemented
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  // Page 91 in UAC2 specification
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  // uint8_t itf = TU_U16_LOW(p_request->wIndex);       // Since we have only one audio function implemented, we do not need the itf value
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  // Input terminal (Microphone input)
  if (entityID == 1) {
    switch (ctrlSel) {
      case AUDIO_TE_CTRL_CONNECTOR: {
        // The terminal connector control only has a get request with only the CUR attribute.
        audio_desc_channel_cluster_t ret;

        // Those are dummy values for now
        ret.bNrChannels = 1;
        ret.bmChannelConfig = (audio_channel_config_t) 0;
        ret.iChannelNames = 0;

        TU_LOG2("    Get terminal connector\r\n");

        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void *) &ret, sizeof(ret));
      } break;

        // Unknown/Unsupported control selector
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  // Feature unit
  if (entityID == 2) {
    switch (ctrlSel) {
      case AUDIO_FU_CTRL_MUTE:
        // Audio control mute cur parameter block consists of only one byte - we thus can send it right away
        // There does not exist a range parameter block for mute
        TU_LOG2("    Get Mute of channel: %u\r\n", channelNum);
        return tud_control_xfer(rhport, p_request, &mute[channelNum], 1);

      case AUDIO_FU_CTRL_VOLUME:
        switch (p_request->bRequest) {
          case AUDIO_CS_REQ_CUR:
            TU_LOG2("    Get Volume of channel: %u\r\n", channelNum);
            return tud_control_xfer(rhport, p_request, &volume[channelNum], sizeof(volume[channelNum]));

          case AUDIO_CS_REQ_RANGE:
            TU_LOG2("    Get Volume range of channel: %u\r\n", channelNum);

            // Copy values - only for testing - better is version below
            audio_control_range_2_n_t(1)
                ret;

            ret.wNumSubRanges = 1;
            ret.subrange[0].bMin = tu_htole16(0x8001); // -90 dB
            ret.subrange[0].bMax = tu_htole16(0x7FFF); // +90 dB
            ret.subrange[0].bRes = tu_htole16(0x0001); // 1 dB steps

            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void *) &ret, sizeof(ret));

            // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
        break;

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  // Clock Source unit
  if (entityID == 4) {
    switch (ctrlSel) {
      case AUDIO_CS_CTRL_SAM_FREQ:
        // channelNum is always zero in this case
        switch (p_request->bRequest) {
          case AUDIO_CS_REQ_CUR:
            TU_LOG2("    Get Sample Freq.\r\n");
            {
              audio_control_cur_4_t curf = { (int32_t)tu_htole32(sampFreq) };
              // Buffered control transfer is needed for IN flow control to work
              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &curf, sizeof(curf));
            }

          case AUDIO_CS_REQ_RANGE:
            TU_LOG2("    Get Sample Freq. range\r\n");
            return tud_control_xfer(rhport, p_request, &sampleFreqRng, sizeof(sampleFreqRng));

            // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
        break;

      case AUDIO_CS_CTRL_CLK_VALID:
        // Only cur attribute exists for this request
        TU_LOG2("    Get Sample Freq. valid\r\n");
        return tud_control_xfer(rhport, p_request, &clkValid, sizeof(clkValid));

      // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  TU_LOG2("  Unsupported entity: %d\r\n", entityID);
  return false;// Yet not implemented
}

bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
{
  (void) rhport;
  (void) itf;
  (void) ep_in;
  (void) cur_alt_setting;

#if USB_AUDIO_DEBUG_STAGE >= 4
  debug_s6_tx_pre_count++;
#endif

  on_usb_microphone_tx_pre_load(rhport, itf, ep_in, cur_alt_setting);

  return true;
}

bool tud_audio_tx_done_post_load_cb(uint8_t rhport, uint16_t n_bytes_copied, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
{
  (void) rhport;
  (void) n_bytes_copied;
  (void) itf;
  (void) ep_in;
  (void) cur_alt_setting;

#if USB_AUDIO_DEBUG_STAGE >= 4
  debug_s6_tx_post_count++;
#endif

  on_usb_microphone_tx_post_load(rhport, n_bytes_copied, itf, ep_in, cur_alt_setting);

  return true;
}

#if CFG_TUD_AUDIO
// Alt-setting change probe: invoked on SET_INTERFACE for the audio streaming interface
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  uint8_t alt = (uint8_t)p_request->wValue;
  audio_stream_alt_setting = alt;
#if USB_AUDIO_DEBUG_STAGE >= 4
  debug_s6_cur_alt = (int8_t)alt;
  debug_s6_alt_set_count++;
#endif

  if (alt == 0u) {
    if ((pendingSampFreq != 0u) && (pendingSampFreq != sampFreq)) {
      sampFreq = pendingSampFreq;
      usb_microphone_current_sample_rate_handler(pendingSampFreq);
      TU_LOG2("    Applied deferred Sample Freq %lu on alt=0\r\n", (unsigned long)pendingSampFreq);
      pendingSampFreq = 0;
    }

    // Stream stopped: reset packet-framing accumulator to restart cleanly.
    usb_tx_frame_samples = 0;
    usb_frame_accumulator = 0;
    ring_prefill_gate = 0u;
  } else {
    // alt=1: arm pre-fill gate so ring accumulates ~20 ms before USB reads begin.
    ring_prefill_gate = RING_PREFILL_CALLBACKS;
  }

  return true;
}
#endif

//--------------------------------------------------------------------+
// BLINKING TASK
//--------------------------------------------------------------------+
void led_blinking_task(void) {
  static uint32_t start_ms = 0;
  static bool led_state = false;

  // Blink every interval ms
  if (board_millis() - start_ms < microphone_settings.blink_interval_ms) return;// not enough time
  start_ms += microphone_settings.blink_interval_ms;

  board_led_write(led_state);
  led_state = 1 - led_state;// toggle
}

//-------------------------
// callback functions
//-------------------------
void usb_microphone_mute_handler(int8_t bChannelNumber, int8_t mute_in)
{
  microphone_settings.mute[bChannelNumber] = mute_in;
  microphone_settings.volume_db[bChannelNumber] = vol_to_db_convert(microphone_settings.mute[bChannelNumber], microphone_settings.volume[bChannelNumber]);

  for(int i=0; i<(CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX); i++) {
    microphone_settings.volume_mul_db[i] = microphone_settings.volume_db[0]
      * microphone_settings.volume_db[i+1];
  }

  microphone_settings.status_updated = true;
}

void usb_microphone_volume_handler(int8_t bChannelNumber, int16_t volume_in)
{
  // If value in range -91 to 0, apply as is
  if((volume_in >= -91) && (volume_in <= 0))
    microphone_settings.volume[bChannelNumber] = volume_in;
   else { // Need to convert the value
     int16_t volume_tmp = volume_in >> ENC_NUM_OF_FP_BITS; // Value in range -128 to 127
     volume_tmp = volume_tmp - 127; // Value in range -255 to 0. Need to have -91 to 0
     volume_tmp = (volume_tmp*91)/255;
     microphone_settings.volume[bChannelNumber] = volume_tmp;    
  }
  microphone_settings.volume_db[bChannelNumber] = vol_to_db_convert(microphone_settings.mute[bChannelNumber], microphone_settings.volume[bChannelNumber]);

  for(int i=0; i<(CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX); i++) {
    microphone_settings.volume_mul_db[i] = microphone_settings.volume_db[0]
      * microphone_settings.volume_db[i+1];
  }

  microphone_settings.status_updated = true;
}

void usb_microphone_current_sample_rate_handler(uint32_t current_sample_rate_in)
{
  microphone_settings.sample_rate = current_sample_rate_in;
  usb_frame_accumulator = 0;
#if USB_AUDIO_DEBUG_STAGE >= 4
  refresh_i2s_connections();
#endif
  microphone_settings.status_updated = true;
}

void usb_microphone_current_resolution_handler(uint8_t current_resolution_in)
{
  microphone_settings.resolution = current_resolution_in;
#if USB_AUDIO_DEBUG_STAGE >= 4
  refresh_i2s_connections();
#endif
  microphone_settings.status_updated = true;
}

void usb_microphone_current_status_set_handler(uint32_t blink_interval_ms_in)
{
  microphone_settings.blink_interval_ms = blink_interval_ms_in;
  microphone_settings.status_updated = true;
}

void on_usb_microphone_tx_pre_load(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
{
  (void) rhport;
  (void) itf;
  (void) ep_in;
  (void) cur_alt_setting;

  /* RP2040 inmp441_6ch_usb: pre-load pushes the buffer filled by the *previous* post-load.
   * Byte count matches usb_tx_frame_samples set at end of last usb_audio_post_load_fill(). */
#if CFG_TUD_AUDIO_ENABLE_ENCODING
  uint16_t ns = usb_tx_frame_samples;
  if (ns == 0) {
    ns = (uint16_t)(sampFreq / 1000u);
  }
  uint32_t frame_bytes_per_fifo = (uint32_t)CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX
      * (uint32_t)CFG_TUD_AUDIO_FUNC_1_CHANNEL_PER_FIFO_TX * (uint32_t)ns;

  for (uint8_t cnt = 0; cnt < CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO; cnt++) {
    tud_audio_write_support_ff(cnt, i2s_dummy_buffer[cnt], frame_bytes_per_fifo);
  }
#else
  uint16_t ns = usb_tx_frame_samples ? usb_tx_frame_samples : (uint16_t)(sampFreq / 1000u);
  tud_audio_write(i2s_dummy_buffer,
      (uint32_t)CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX * (uint32_t)CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX * (uint32_t)ns);
#endif
}

void on_usb_microphone_tx_post_load(uint8_t rhport, uint16_t n_bytes_copied, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
{
  (void) rhport;
  (void) n_bytes_copied;
  (void) itf;
  (void) ep_in;
  (void) cur_alt_setting;
  usb_audio_post_load_fill();
}

uint16_t get_usb_frame_samples(void) {
    uint32_t current_rate = sampFreq;
    uint16_t samples = (uint16_t)(current_rate / 1000u);

    usb_frame_accumulator += (current_rate % 1000u);
    if (usb_frame_accumulator >= 1000u) {
        samples++;
        usb_frame_accumulator -= 1000u;
    }

    return samples;
}

static void usb_audio_post_load_fill(void) {
#if USB_AUDIO_DEBUG_STAGE >= 4
    debug_s6_prepare_called++;
#endif

    int32_t buffer[4 * 2 * USB_FRAME_SAMPLES_MAX];
    usb_tx_frame_samples = get_usb_frame_samples();
    uint16_t frames_to_copy = usb_tx_frame_samples;
#if USB_AUDIO_DEBUG_STAGE == 6
    uint16_t peak[6] = {0};
#endif

#if CFG_TUD_AUDIO_ENABLE_ENCODING
    memset(i2s_dummy_buffer, 0, sizeof(i2s_dummy_buffer));
#else
    memset(i2s_dummy_buffer, 0, sizeof(i2s_dummy_buffer));
#endif

    if (!i2s0) {
#if USB_AUDIO_DEBUG_STAGE >= 4
        debug_s6_i2s_null = 1;
#endif
        return;
    }

    // Pre-fill gate: hold USB fill path while ring is warming up after stream start.
    // Decrement each callback; once expired, normal reading begins.
    if (ring_prefill_gate > 0u) {
        ring_prefill_gate--;
        return;
    }

    // Read exactly the bytes needed for this USB frame — no more, no less.
    // sizeof(buffer) = 1568 B (49 frames × 32 B). At 44.1 kHz only 44–45 frames are
    // needed (1408–1440 B). Requesting 1568 B over-consumes the ring by ~160 B/ms
    // against a DMA fill rate of 1411 B/ms, draining the entire pre-fill buffer in
    // ~180 ms and restarting starvation. Requesting exactly usb_tx_frame_samples×32
    // keeps USB drain ≈ DMA fill rate, so the ring stays full and starvation stops.
    size_t request_bytes = (size_t)usb_tx_frame_samples * (4u * I2S_RX_FRAME_SIZE_IN_BYTES);
    int num_bytes_read = microphone_array_i2s_read_stream_nonblocking(i2s0, (void *)&buffer[0], request_bytes);
#if USB_AUDIO_DEBUG_STAGE == 6
    debug_stage6_last_num_bytes_read = num_bytes_read;
#endif
    if (num_bytes_read < (4 * I2S_RX_FRAME_SIZE_IN_BYTES)) {
        // No new I2S data — fill with last good sample then apply DC-blocking
        // exponential decay (>> 4 per callback ≈ 16 ms to near-zero at 1 kHz
        // callback rate). This removes hard silence edges (ring-mod source)
        // while preventing a sustained DC offset during long starvation periods.
#if CFG_TUD_AUDIO_ENABLE_ENCODING
        usb_tx_sample_t *fifo0 = i2s_dummy_buffer[0];
        usb_tx_sample_t *fifo1 = i2s_dummy_buffer[1];
        usb_tx_sample_t *fifo2 = i2s_dummy_buffer[2];
        uint16_t ns = usb_tx_frame_samples ? usb_tx_frame_samples : (uint16_t)(sampFreq / 1000u);
        for (uint16_t i = 0; i < ns; i++) {
            fifo0[i * 2 + 0] = last_good_sample[0];
            fifo0[i * 2 + 1] = last_good_sample[1];
            fifo1[i * 2 + 0] = last_good_sample[2];
            fifo1[i * 2 + 1] = last_good_sample[3];
            fifo2[i * 2 + 0] = last_good_sample[4];
            fifo2[i * 2 + 1] = last_good_sample[5];
        }
        // Decay toward zero: each callback reduces sample by ~6.25%.
        for (int ch = 0; ch < 6; ch++) {
            last_good_sample[ch] = (usb_tx_sample_t)(
                (int32_t)last_good_sample[ch] - ((int32_t)last_good_sample[ch] >> 4));
        }
        // Arm soft re-entry crossfade so the next live callback blends smoothly
        // from the decayed held value back to live audio, eliminating recovery clicks.
        recovery_blend_remaining = (uint8_t)RECOVERY_BLEND_LEN;
#endif
#if USB_AUDIO_DEBUG_STAGE == 6
        debug_stage6_last_frames_to_copy = 0;
        for (int ch = 0; ch < 6; ch++) {
            debug_stage6_peak[ch] = 0;
        }
#endif
        return;
    }

    int num_of_frames_read = num_bytes_read / (4 * I2S_RX_FRAME_SIZE_IN_BYTES);
    if (num_of_frames_read < frames_to_copy) {
        frames_to_copy = (uint16_t)num_of_frames_read;
    }
    // usb_tx_frame_samples stays at the requested count — packet size is always fixed.
    // Any frames beyond frames_to_copy are padded with last_good_sample (see tail-fill below),
    // removing the variable-length USB packet rhythm that drives the 4 Hz AM artifacts.
#if USB_AUDIO_DEBUG_STAGE == 6
    debug_stage6_last_frames_to_copy = frames_to_copy;
#endif

    i2s_audio_sample *sample_ptr = (i2s_audio_sample *)buffer;

#if CFG_TUD_AUDIO_ENABLE_ENCODING
    usb_tx_sample_t *fifo0 = i2s_dummy_buffer[0];
#if USB_AUDIO_DEBUG_STAGE >= 5
    usb_tx_sample_t *fifo1 = i2s_dummy_buffer[1];
    usb_tx_sample_t *fifo2 = i2s_dummy_buffer[2];
    usb_tx_sample_t *fifo3 = i2s_dummy_buffer[3];
#endif

    for (uint32_t i = 0; i < frames_to_copy; i++) {
        i2s_audio_sample sample = decode_sample(&(sample_ptr[i]));
#if USB_AUDIO_DEBUG_STAGE == 4
        fifo0[i * 2 + 0] = i2s_to_usb_sample_convert(sample.sample_l[0], microphone_settings.volume_mul_db[0]);
        fifo0[i * 2 + 1] = i2s_to_usb_sample_convert(sample.sample_r[0], microphone_settings.volume_mul_db[1]);
#else
        {
        // Soft re-entry blend: linearly crossfade from decayed last_good_sample to
        // live decoded value over the first RECOVERY_BLEND_LEN frames after starvation.
        // blend weight r: 32=fully held, 0=fully live. Shift by 5 (divide by 32).
        uint8_t r = recovery_blend_remaining;
        usb_tx_sample_t live0 = i2s_to_usb_sample_convert(sample.sample_l[0], microphone_settings.volume_mul_db[0]);
        usb_tx_sample_t live1 = i2s_to_usb_sample_convert(sample.sample_r[0], microphone_settings.volume_mul_db[1]);
        usb_tx_sample_t live2 = i2s_to_usb_sample_convert(sample.sample_l[1], microphone_settings.volume_mul_db[2]);
        usb_tx_sample_t live3 = i2s_to_usb_sample_convert(sample.sample_r[1], microphone_settings.volume_mul_db[3]);
        usb_tx_sample_t live4 = i2s_to_usb_sample_convert(sample.sample_l[2], microphone_settings.volume_mul_db[4]);
        usb_tx_sample_t live5 = i2s_to_usb_sample_convert(sample.sample_r[2], microphone_settings.volume_mul_db[5]);
        if (r > 0u) {
            uint8_t live_w = (uint8_t)(RECOVERY_BLEND_LEN - r);
            fifo0[i * 2 + 0] = (usb_tx_sample_t)(((int32_t)last_good_sample[0] * r + (int32_t)live0 * live_w) >> 5);
            fifo0[i * 2 + 1] = (usb_tx_sample_t)(((int32_t)last_good_sample[1] * r + (int32_t)live1 * live_w) >> 5);
            fifo1[i * 2 + 0] = (usb_tx_sample_t)(((int32_t)last_good_sample[2] * r + (int32_t)live2 * live_w) >> 5);
            fifo1[i * 2 + 1] = (usb_tx_sample_t)(((int32_t)last_good_sample[3] * r + (int32_t)live3 * live_w) >> 5);
            fifo2[i * 2 + 0] = (usb_tx_sample_t)(((int32_t)last_good_sample[4] * r + (int32_t)live4 * live_w) >> 5);
            fifo2[i * 2 + 1] = (usb_tx_sample_t)(((int32_t)last_good_sample[5] * r + (int32_t)live5 * live_w) >> 5);
            recovery_blend_remaining--;
        } else {
            fifo0[i * 2 + 0] = live0;
            fifo0[i * 2 + 1] = live1;
            fifo1[i * 2 + 0] = live2;
            fifo1[i * 2 + 1] = live3;
            fifo2[i * 2 + 0] = live4;
            fifo2[i * 2 + 1] = live5;
        }
        fifo3[i * 2 + 0] = 0;
        fifo3[i * 2 + 1] = 0;
        last_good_sample[0] = fifo0[i * 2 + 0];
        last_good_sample[1] = fifo0[i * 2 + 1];
        last_good_sample[2] = fifo1[i * 2 + 0];
        last_good_sample[3] = fifo1[i * 2 + 1];
        last_good_sample[4] = fifo2[i * 2 + 0];
        last_good_sample[5] = fifo2[i * 2 + 1];
        }
#if USB_AUDIO_DEBUG_STAGE == 6
        {
          int32_t s1 = (int32_t)sample.sample_l[0];
          int32_t s2 = (int32_t)sample.sample_r[0];
          int32_t s3 = (int32_t)sample.sample_l[1];
          int32_t s4 = (int32_t)sample.sample_r[1];
          int32_t s5 = (int32_t)sample.sample_l[2];
          int32_t s6 = (int32_t)sample.sample_r[2];
          uint16_t raw_level[6] = {
              (uint16_t)((s1 < 0) ? -s1 : s1),
              (uint16_t)((s2 < 0) ? -s2 : s2),
              (uint16_t)((s3 < 0) ? -s3 : s3),
              (uint16_t)((s4 < 0) ? -s4 : s4),
              (uint16_t)((s5 < 0) ? -s5 : s5),
              (uint16_t)((s6 < 0) ? -s6 : s6),
          };
          for (int ch = 0; ch < 6; ch++) {
            if (raw_level[ch] > peak[ch]) {
              peak[ch] = raw_level[ch];
            }
          }
        }
#endif
#endif
    }

    // Tail-fill: pad frames [frames_to_copy .. usb_tx_frame_samples) with last_good_sample.
    // When partial starvation occurs (ring drained, not fully empty), this keeps the
    // USB packet at full size rather than sending a short packet that creates a
    // variable-length rhythm at the host (a primary source of the 4 Hz AM artifact).
    for (uint32_t i = frames_to_copy; i < usb_tx_frame_samples; i++) {
        fifo0[i * 2 + 0] = last_good_sample[0];
        fifo0[i * 2 + 1] = last_good_sample[1];
#if USB_AUDIO_DEBUG_STAGE >= 5
        fifo1[i * 2 + 0] = last_good_sample[2];
        fifo1[i * 2 + 1] = last_good_sample[3];
        fifo2[i * 2 + 0] = last_good_sample[4];
        fifo2[i * 2 + 1] = last_good_sample[5];
        fifo3[i * 2 + 0] = 0;
        fifo3[i * 2 + 1] = 0;
#endif
    }
#else
    usb_tx_sample_t *usb_buffer = i2s_dummy_buffer;
    for (uint32_t i = 0; i < frames_to_copy; i++) {
        i2s_audio_sample sample = decode_sample(&(sample_ptr[i]));
#if USB_AUDIO_DEBUG_STAGE == 4
        usb_buffer[i * 2 + 0] = i2s_to_usb_sample_convert(sample.sample_l[0], microphone_settings.volume_mul_db[0]);
        usb_buffer[i * 2 + 1] = i2s_to_usb_sample_convert(sample.sample_r[0], microphone_settings.volume_mul_db[1]);
#else
        usb_buffer[i * 8 + 0] = i2s_to_usb_sample_convert(sample.sample_l[0], microphone_settings.volume_mul_db[0]);
        usb_buffer[i * 8 + 1] = i2s_to_usb_sample_convert(sample.sample_r[0], microphone_settings.volume_mul_db[1]);
        usb_buffer[i * 8 + 2] = i2s_to_usb_sample_convert(sample.sample_l[1], microphone_settings.volume_mul_db[2]);
        usb_buffer[i * 8 + 3] = i2s_to_usb_sample_convert(sample.sample_r[1], microphone_settings.volume_mul_db[3]);
        usb_buffer[i * 8 + 4] = i2s_to_usb_sample_convert(sample.sample_l[2], microphone_settings.volume_mul_db[4]);
        usb_buffer[i * 8 + 5] = i2s_to_usb_sample_convert(sample.sample_r[2], microphone_settings.volume_mul_db[5]);
        usb_buffer[i * 8 + 6] = 0;
        usb_buffer[i * 8 + 7] = 0;
#endif
    }
#endif
#if USB_AUDIO_DEBUG_STAGE == 6
    for (int ch = 0; ch < 6; ch++) {
        debug_stage6_peak[ch] = peak[ch];
    }
#endif
}

usb_tx_sample_t i2s_to_usb_sample_convert(uint32_t sample, uint32_t volume_db) {
    (void) volume_db;
#if USB_AUDIO_DEBUG_STAGE == 4 || USB_AUDIO_DEBUG_STAGE == 7
    return sample;
#else
    return (usb_tx_sample_t)(((int32_t)sample) >> 16);
#endif
}

#if USB_AUDIO_DEBUG_STAGE == 6
void debug_stage6_cdc_levels_task(void) {
  static uint32_t last_report_ms = 0;
  uint32_t now = board_millis();

  if (!tud_cdc_connected() || (now - last_report_ms) < 200) {
    return;
  }

  // Stage 8: init error code
  int8_t s6_err = get_last_i2s_init_error();
  // Stage 9: DMA IRQ fire counter
  uint32_t s6_irq = get_i2s_dma_irq_count();
  // Stage 11: DMA ring overflow telemetry
  uint32_t s6_ovr = get_i2s_dma_overrun_count();
  uint32_t s6_ovr_bytes = get_i2s_dma_overrun_bytes();
  // Stage 10: PIO FIFO level and SM enabled state
  uint8_t s6_fifo = 0;
  uint8_t s6_sm   = 0;
  uint16_t s6_div_int = 0;
  uint8_t s6_div_frac = 0;
  uint32_t s6_rate = microphone_settings.sample_rate;
  if (i2s0 != NULL) {
    s6_fifo = (uint8_t)pio_sm_get_rx_fifo_level(i2s0->pio, i2s0->sm);
    s6_sm   = (uint8_t)((i2s0->pio->ctrl >> (PIO_CTRL_SM_ENABLE_LSB + i2s0->sm)) & 1u);
    s6_div_int = i2s0->pio_clkdiv_int;
    s6_div_frac = i2s0->pio_clkdiv_frac;
  }

  char line[256];
  int written = snprintf(
      line,
      sizeof(line),
      "prep=%lu null=%u err=%d irq=%lu ovr=%lu ovr_bytes=%lu rate=%lu div=%u.%u altcnt=%lu alt=%d txpre=%lu txpost=%lu fifo=%u sm=%u bytes=%d frames=%u ch1=%u ch2=%u ch3=%u ch4=%u ch5=%u ch6=%u\r\n",
      (unsigned long)debug_s6_prepare_called,
      debug_s6_i2s_null,
      (int)s6_err,
      (unsigned long)s6_irq,
      (unsigned long)s6_ovr,
      (unsigned long)s6_ovr_bytes,
      (unsigned long)s6_rate,
      (unsigned int)s6_div_int,
      (unsigned int)s6_div_frac,
      (unsigned long)debug_s6_alt_set_count,
      (int)debug_s6_cur_alt,
      (unsigned long)debug_s6_tx_pre_count,
      (unsigned long)debug_s6_tx_post_count,
      s6_fifo,
      s6_sm,
      debug_stage6_last_num_bytes_read,
      debug_stage6_last_frames_to_copy,
      debug_stage6_peak[0],
      debug_stage6_peak[1],
      debug_stage6_peak[2],
      debug_stage6_peak[3],
      debug_stage6_peak[4],
      debug_stage6_peak[5]);

  if (written > 0) {
    tud_cdc_write(line, (uint32_t)written);
    tud_cdc_write_flush();
  }

  last_report_ms = now;
}
#endif
#else
int main(void) {
  static uint32_t last_report_ms = 0;

  set_sys_clock_khz(AUDIO_F_SYS_KHZ, true);  // 192 MHz before board_init()
  debug_stage1_boot_probe();
  stdio_init_all();
  board_init();
  tud_init(BOARD_TUD_RHPORT);
  if (board_init_after_tusb) {
    board_init_after_tusb();
  }
  debug_stage2_post_tusb_probe();

  while (1) {
    tud_task();
    debug_stage2_loop_probe();

    if (tud_cdc_connected() && (board_millis() - last_report_ms) >= 1000) {
      tud_cdc_write_str("usb_audio_debug_stage3 alive\r\n");
      tud_cdc_write_flush();
      last_report_ms = board_millis();
    }
  }
}
#endif
