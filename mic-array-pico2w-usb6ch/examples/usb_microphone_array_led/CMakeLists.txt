cmake_minimum_required(VERSION 3.13)

# Application-layer build file.
# Board target is inherited from root CMakeLists.txt (LAYER 1).
# Do not set PICO_BOARD, PICO_PLATFORM, or clock constants here.

add_executable(usb_microphone_array_led
    main.c
    usb_descriptors.c
    usb_microphone.c
)

target_include_directories(usb_microphone_array_led PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(usb_microphone_array_led PRIVATE
    tinyusb_device
    tinyusb_board
    pico_stdlib
    hardware_clocks       # for set_sys_clock_khz() and clock_get_hz()
    pico_microphone_array_i2s
    pico_9822
    pico_9822_led_animation
    pico_volume_ctrl
)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(usb_microphone_array_led)
