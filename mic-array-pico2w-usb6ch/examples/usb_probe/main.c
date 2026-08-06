#include <stdio.h>

#include "pico/stdlib.h"

int main(void) {
    stdio_init_all();

    // Give the host a stable USB CDC device to enumerate.
    while (true) {
        sleep_ms(1000);
        printf("usb_probe alive\n");
    }
}
