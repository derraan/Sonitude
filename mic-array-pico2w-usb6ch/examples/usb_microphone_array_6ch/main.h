#ifndef MAIN__H
#define MAIN__H

#include <stdint.h>

//-------------------------
// I2s defines
//-------------------------
#ifndef PIN_SCK
    #define PIN_SCK 0
#endif //PIN_SCK

#ifndef PIN_WS
    #define PIN_WS (PIN_SCK+1) // needs to be PIN_SCK +1
#endif //PIN_WS

#ifndef PIN_SD0
    #define PIN_SD0 (PIN_WS + 1) // Can be different
#endif //PIN_SD0

#ifndef PIN_SD1
    #define PIN_SD1 (PIN_SD0 + 1)  // needs to be PIN_SD0 +1
#endif //PIN_SD1

#ifndef PIN_SD2
    #define PIN_SD2 (PIN_SD1 + 1)  // needs to be PIN_SD1 +1
#endif //PIN_SD2

#ifndef I2S_BPS
    #define I2S_BPS 32 // INMP441 capture stays 32-bit in the I2S path; USB payload width may be narrower.
#endif //I2S_BPS


#ifndef RATE
    #define RATE (CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE)
#endif //RATE

typedef int32_t usb_audio_sample;

//-------------------------

#endif //MAIN__H
