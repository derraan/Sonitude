/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
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

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#include "usb_descriptors.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef USB_AUDIO_DEBUG_STAGE
#define USB_AUDIO_DEBUG_STAGE 5
#endif

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+

// RHPort number used for device can be defined by board.mk, default to port 0
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

// RHPort max operational speed can defined by board.mk
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// defined by compiler flags for flexibility
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG            0
#endif

// Enable Device stack
#define CFG_TUD_ENABLED           1

// Default is max speed that hardware controller could support with on-chip PHY
#define CFG_TUD_MAX_SPEED         BOARD_TUD_MAX_SPEED

/* USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
 * Tinyusb use follows macros to declare transferring memory so that they can be put
 * into those specific section.
 * e.g
 * - CFG_TUSB_MEM SECTION : __attribute__ (( section(".usb_ram") ))
 * - CFG_TUSB_MEM_ALIGN   : __attribute__ ((aligned(4)))
 */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
#if USB_AUDIO_DEBUG_STAGE == 3
#define CFG_TUD_AUDIO             0
#define CFG_TUD_CDC               1
#elif USB_AUDIO_DEBUG_STAGE == 6
#define CFG_TUD_AUDIO             1
#define CFG_TUD_CDC               1
#else
#define CFG_TUD_AUDIO             1
#define CFG_TUD_CDC               0
#endif
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

#if CFG_TUD_CDC
#define CFG_TUD_CDC_RX_BUFSIZE    (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_TX_BUFSIZE    (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_EP_BUFSIZE    (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------

// Have a look into audio_device.h for all configurations
#if CFG_TUD_AUDIO
// Optional compile-time override for production bring-up, e.g.
// -DCFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE_OVERRIDE=24000
// Supported values in this firmware path: 16000, 24000, 44100, 48000.
#ifdef CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE_OVERRIDE
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE              CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE_OVERRIDE
#else
#if USB_AUDIO_DEBUG_STAGE == 4 || USB_AUDIO_DEBUG_STAGE == 7
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE              16000
#else
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE              44100
#endif
#endif

#if (CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE != 16000) && \
    (CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE != 24000) && \
    (CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE != 44100) && \
    (CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE != 48000)
#error Unsupported CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE. Use 16000, 24000, 44100, or 48000.
#endif

#if USB_AUDIO_DEBUG_STAGE == 4
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                 TUD_AUDIO_MIC_TWO_CH_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX            2
#else
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                 TUD_AUDIO_MIC_EIGHT_CH_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX            8
#endif

#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ              64

#define CFG_TUD_AUDIO_ENABLE_EP_IN                    1
#if USB_AUDIO_DEBUG_STAGE == 4 || USB_AUDIO_DEBUG_STAGE == 7
// Stage 4/7 stay in 32-bit USB slots for raw-path debug.
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX    4
#elif USB_AUDIO_DEBUG_STAGE == 5 && defined(USB_AUDIO_STAGE5_32BIT) && (USB_AUDIO_STAGE5_32BIT)
// Optional Stage 5 raw-width debug mode: keep Stage 5 descriptor shape/stability,
// but send 32-bit USB samples to inspect alignment artifacts without Stage 7.
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX    4
#else
// Stage 5/6 use 16-bit USB payload to stay within full-speed isoch packet limits at 44.1kHz/8ch.
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX    2
#endif
// Endpoint max packet must cover the HIGHEST selectable runtime rate (48 kHz),
// not the compile-time default. The descriptor is fixed at enumeration time; if
// the host switches to 48 kHz the required packet size is 784 bytes (8ch x 2B x 49
// samples). If this is sized for the default (e.g. 44.1 kHz = 720B), the host
// rejects the stream open with paInvalidDevice / error -9996.
// AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK permits shorter packets at lower
// rates so oversizing the endpoint is both safe and required.
#define CFG_TUD_AUDIO_EP_SZ_IN                        TUD_AUDIO_EP_SIZE(48000, CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)

#define CFG_TUD_AUDIO_ENABLE_ENCODING                 1
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL              1

#if CFG_TUD_AUDIO_ENABLE_ENCODING

#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX             CFG_TUD_AUDIO_EP_SZ_IN
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ          CFG_TUD_AUDIO_EP_SZ_IN

#define CFG_TUD_AUDIO_ENABLE_TYPE_I_ENCODING          1
#define CFG_TUD_AUDIO_FUNC_1_CHANNEL_PER_FIFO_TX      2         // One I2S stream contains two channels, each stream is saved within one support FIFO - this value is currently fixed, the driver does not support a changing value
#if USB_AUDIO_DEBUG_STAGE == 4
#define CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO        1
#else
#define CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO        (CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX / CFG_TUD_AUDIO_FUNC_1_CHANNEL_PER_FIFO_TX)
#endif
#define CFG_TUD_AUDIO_FUNC_1_TX_SUPP_SW_FIFO_SZ       (TUD_OPT_HIGH_SPEED ? 32 : 4) * (CFG_TUD_AUDIO_EP_SZ_IN / CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO) // Example write FIFO every 1ms, so it should be 8 times larger for HS device

#else

#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX             CFG_TUD_AUDIO_EP_SZ_IN
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ          (TUD_OPT_HIGH_SPEED ? 32 : 4) * CFG_TUD_AUDIO_EP_SZ_IN // Example write FIFO every 1ms, so it should be 8 times larger for HS device

#endif
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT                 1
#endif

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
