cmake_minimum_required(VERSION 3.13)

project(usb_probe)

add_executable(usb_probe
    main.c
)

target_link_libraries(usb_probe PRIVATE pico_stdlib)

pico_enable_stdio_usb(usb_probe 1)
pico_enable_stdio_uart(usb_probe 0)

pico_add_extra_outputs(usb_probe)
