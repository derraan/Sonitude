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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "microphone_settings.h"

#include "pico/microphone_array_i2s.h"
#include "pico/volume_ctrl.h"

#include "bsp/board_api.h"
#include "tusb.h"
#include "tusb_config.h"

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+
#define AUDIO_SAMPLE_RATE CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE
#define SAMPLE_BUFFER_SIZE  (AUDIO_SAMPLE_RATE/1000)

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

// Range states
audio_control_range_2_n_t(1) volumeRng[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];// Volume range state
audio_control_range_4_n_t(1) sampleFreqRng;                                    // Sample frequency range state

#if CFG_TUD_AUDIO_ENABLE_ENCODING
// Audio test data, each buffer contains 2 channels, buffer[0] for CH0-1, buffer[1] for CH1-2
uint32_t i2s_dummy_buffer[CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO][CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX*(CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE/1000)/CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO];
#else
// Audio test data, 4 channels muxed together, buffer[0] for CH0, buffer[1] for CH1, buffer[2] for CH2, buffer[3] for CH3
uint32_t i2s_dummy_buffer[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX*CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE/1000];
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
uint32_t i2s_to_usb_32b_sample_convert(uint32_t sample, uint32_t volume_db);
//-------------------------

// Pointer to I2S handler
microphone_array_i2s_obj_t* i2s0 = NULL;

void refresh_i2s_connections()
{
  // microphone_settings.samples_in_i2s_frame_min = (microphone_settings.sample_rate)    /1000;
  // microphone_settings.samples_in_i2s_frame_max = (microphone_settings.sample_rate+999)/1000;

  i2s0 = create_microphone_array_i2s(0, PIN_SCK, PIN_SD0, BPS, SIZEOF_DMA_BUFFER_IN_BYTES, RATE);
}

/*------------- MAIN -------------*/
int main(void) {

  stdio_init_all();

  board_init();

  // init device stack on configured roothub port
  tusb_rhport_init_t dev_init = {
      .role = TUSB_ROLE_DEVICE,
      .speed = TUSB_SPEED_AUTO};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);

  board_init_after_tusb();

  refresh_i2s_connections();

  // Init values
  sampFreq = AUDIO_SAMPLE_RATE;
  clkValid = 1;

  sampleFreqRng.wNumSubRanges = 1;
  sampleFreqRng.subrange[0].bMin = AUDIO_SAMPLE_RATE;
  sampleFreqRng.subrange[0].bMax = AUDIO_SAMPLE_RATE;
  sampleFreqRng.subrange[0].bRes = 0;


  while (1) {
    tud_task();// tinyusb device task
    led_blinking_task();
    audio_task();
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
            // Buffered control transfer is needed for IN flow control to work
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sampFreq, sizeof(sampFreq));

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

  on_usb_microphone_tx_post_load(rhport, n_bytes_copied, itf, ep_in, cur_alt_setting);

  return true;
}

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
  refresh_i2s_connections();
  microphone_settings.status_updated = true;
}

void usb_microphone_current_resolution_handler(uint8_t current_resolution_in)
{
  microphone_settings.resolution = current_resolution_in;
  refresh_i2s_connections();
  microphone_settings.status_updated = true;
}

void usb_microphone_current_status_set_handler(uint32_t blink_interval_ms_in)
{
  microphone_settings.blink_interval_ms = blink_interval_ms_in;
  microphone_settings.status_updated = true;
}

void on_usb_microphone_tx_pre_load(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
{
  #if CFG_TUD_AUDIO_ENABLE_ENCODING
  // Write I2S buffer into FIFO
  for (uint8_t cnt=0; cnt < CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO; cnt++)
  {
    //CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX*(CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE/1000)/CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO
    //tud_audio_write_support_ff(cnt, i2s_dummy_buffer[cnt], AUDIO_SAMPLE_RATE/1000 * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX * CFG_TUD_AUDIO_FUNC_1_CHANNEL_PER_FIFO_TX);
    tud_audio_write_support_ff(cnt, i2s_dummy_buffer[cnt], CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX*CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX*(CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE/1000)/CFG_TUD_AUDIO_FUNC_1_N_TX_SUPP_SW_FIFO);
  }
#else
  tud_audio_write(i2s_dummy_buffer, AUDIO_SAMPLE_RATE/1000 * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX);
#endif
}

void on_usb_microphone_tx_post_load(uint8_t rhport, uint16_t n_bytes_copied, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
{
  int32_t buffer[4*2*SAMPLE_BUFFER_SIZE];
  if(i2s0) {
    // Read data from microphone
    int num_bytes_read = microphone_array_i2s_read_stream(i2s0, (void*)&buffer[0], sizeof(buffer));

    if(num_bytes_read >= (4*I2S_RX_FRAME_SIZE_IN_BYTES)) {
      int num_of_frames_read = num_bytes_read/(4*I2S_RX_FRAME_SIZE_IN_BYTES);
      i2s_audio_sample* sample_ptr = (i2s_audio_sample*)buffer;

#if CFG_TUD_AUDIO_ENABLE_ENCODING
      for(uint32_t i = 0; i < num_of_frames_read; i++){
        i2s_audio_sample sample = decode_sample(&(sample_ptr[i]));
        i2s_dummy_buffer[0][i*2+0] = i2s_to_usb_32b_sample_convert((sample.sample_l[0]), microphone_settings.volume_mul_db[0]); // TODO: check this value
        i2s_dummy_buffer[0][i*2+1] = i2s_to_usb_32b_sample_convert((sample.sample_l[1]), microphone_settings.volume_mul_db[1]); // TODO: check this value
        i2s_dummy_buffer[1][i*2+0] = i2s_to_usb_32b_sample_convert((sample.sample_l[2]), microphone_settings.volume_mul_db[2]); // TODO: check this value
        i2s_dummy_buffer[1][i*2+1] = i2s_to_usb_32b_sample_convert((sample.sample_l[3]), microphone_settings.volume_mul_db[3]); // TODO: check this value
        i2s_dummy_buffer[2][i*2+0] = i2s_to_usb_32b_sample_convert((sample.sample_r[0]), microphone_settings.volume_mul_db[4]); // TODO: check this value
        i2s_dummy_buffer[2][i*2+1] = i2s_to_usb_32b_sample_convert((sample.sample_r[1]), microphone_settings.volume_mul_db[5]); // TODO: check this value
        i2s_dummy_buffer[3][i*2+0] = i2s_to_usb_32b_sample_convert((sample.sample_r[2]), microphone_settings.volume_mul_db[6]); // TODO: check this value
        i2s_dummy_buffer[3][i*2+1] = i2s_to_usb_32b_sample_convert((sample.sample_r[3]), microphone_settings.volume_mul_db[7]); // TODO: check this value
      }
#else 
      for(uint32_t i = 0; i < num_of_frames_read; i++){
        i2s_audio_sample sample = decode_sample(&(sample_ptr[i]));
        i2s_dummy_buffer[i*8+0] = i2s_to_usb_32b_sample_convert((sample.sample_l[0]), microphone_settings.volume_mul_db[0]); // TODO: check this value
        i2s_dummy_buffer[i*8+1] = i2s_to_usb_32b_sample_convert((sample.sample_l[1]), microphone_settings.volume_mul_db[1]); // TODO: check this value
        i2s_dummy_buffer[i*8+2] = i2s_to_usb_32b_sample_convert((sample.sample_l[2]), microphone_settings.volume_mul_db[2]); // TODO: check this value
        i2s_dummy_buffer[i*8+3] = i2s_to_usb_32b_sample_convert((sample.sample_l[3]), microphone_settings.volume_mul_db[3]); // TODO: check this value
        i2s_dummy_buffer[i*8+4] = i2s_to_usb_32b_sample_convert((sample.sample_r[0]), microphone_settings.volume_mul_db[4]); // TODO: check this value
        i2s_dummy_buffer[i*8+5] = i2s_to_usb_32b_sample_convert((sample.sample_r[1]), microphone_settings.volume_mul_db[5]); // TODO: check this value
        i2s_dummy_buffer[i*8+6] = i2s_to_usb_32b_sample_convert((sample.sample_r[2]), microphone_settings.volume_mul_db[6]); // TODO: check this value
        i2s_dummy_buffer[i*8+7] = i2s_to_usb_32b_sample_convert((sample.sample_r[3]), microphone_settings.volume_mul_db[7]); // TODO: check this value
      }
#endif
    }
  }
}

uint32_t i2s_to_usb_32b_sample_convert(uint32_t sample, uint32_t volume_db){
    return sample;
}