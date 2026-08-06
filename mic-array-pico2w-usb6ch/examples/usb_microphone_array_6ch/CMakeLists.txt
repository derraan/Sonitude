cmake_minimum_required(VERSION 3.13)

# LAYER 3: Application-layer build file.
# Board target is inherited from root CMakeLists.txt (LAYER 1).
# Do not hardcode PICO_BOARD, PICO_PLATFORM, or clock constants here.

project(usb_mic_array_6ch_pico2w)

set(USB_AUDIO_DEBUG_STAGE "5" CACHE STRING "Sequential USB audio debug stage")
set(USB_AUDIO_SAMPLE_RATE_OVERRIDE "0" CACHE STRING "Optional USB audio sample rate override (0 disables, e.g. 24000)")
set(USB_AUDIO_STAGE5_32BIT "0" CACHE STRING "Stage 5 only: set to 1 for 32-bit USB payload debug mode")

set(USB_MIC_SOURCES
    main.c
)

if(NOT USB_AUDIO_DEBUG_STAGE EQUAL 0)
    list(APPEND USB_MIC_SOURCES
        usb_descriptors.c
    )
endif()

add_executable(usb_mic_array_6ch_pico2w
    ${USB_MIC_SOURCES}
)

if(NOT USB_AUDIO_DEBUG_STAGE EQUAL 0)
    target_include_directories(usb_mic_array_6ch_pico2w PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
    )
endif()

target_link_libraries(usb_mic_array_6ch_pico2w PRIVATE
    pico_stdlib
)

if(USB_AUDIO_DEBUG_STAGE EQUAL 0)
    pico_enable_stdio_usb(usb_mic_array_6ch_pico2w 1)
    pico_enable_stdio_uart(usb_mic_array_6ch_pico2w 0)
else()
    target_link_libraries(usb_mic_array_6ch_pico2w PRIVATE
        tinyusb_device
        tinyusb_board
    )

    if(NOT USB_AUDIO_DEBUG_STAGE EQUAL 3)
        target_link_libraries(usb_mic_array_6ch_pico2w PRIVATE
            pico_microphone_array_i2s
            pico_volume_ctrl
            hardware_clocks       # for set_sys_clock_khz() and clock_get_hz()
        )
    else()
        # Stage 3 (CDC-only debug): still needs set_sys_clock_khz().
        target_link_libraries(usb_mic_array_6ch_pico2w PRIVATE
            hardware_clocks
        )
    endif()
endif()

target_compile_definitions(usb_mic_array_6ch_pico2w PRIVATE
    USB_AUDIO_DEBUG_STAGE=${USB_AUDIO_DEBUG_STAGE}
)

if(NOT USB_AUDIO_SAMPLE_RATE_OVERRIDE STREQUAL "0")
    target_compile_definitions(usb_mic_array_6ch_pico2w PRIVATE
        CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE_OVERRIDE=${USB_AUDIO_SAMPLE_RATE_OVERRIDE}
    )
endif()

if(USB_AUDIO_STAGE5_32BIT STREQUAL "1")
    target_compile_definitions(usb_mic_array_6ch_pico2w PRIVATE
        USB_AUDIO_STAGE5_32BIT=1
    )
endif()

pico_add_extra_outputs(usb_mic_array_6ch_pico2w)
