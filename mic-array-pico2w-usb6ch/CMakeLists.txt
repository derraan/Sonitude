# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.0)
set(toolchainVersion 13_2_Rel1)
set(picotoolVersion 2.1.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# LAYER 1: Hardware/Environment target — the only place board is specified.
# pico2_w is required for the Raspberry Pi Pico 2W target (RP2350 + CYW43).
# Board definition was added in SDK 2.1.0; do not downgrade below that.
set(PICO_BOARD pico2_w)

# pico_sdk_import.cmake is copied from the Pico SDK.
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(pico_microphone)

# Work around RP2350 SW spin-lock codepath/toolchain mismatch seen on Windows.
# This keeps the build on hardware spin locks for this project.
set(PICO_USE_SW_SPIN_LOCKS 0 CACHE BOOL "Use HW spin locks in this project" FORCE)
add_compile_definitions(PICO_USE_SW_SPIN_LOCKS=0)

# initialize the Pico SDK
pico_sdk_init()

###################################################
# Volume control library
###################################################
add_library(pico_volume_ctrl INTERFACE)

target_sources(pico_volume_ctrl INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/volume_ctrl.c
)

target_include_directories(pico_volume_ctrl INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(pico_volume_ctrl INTERFACE pico_stdlib)
###################################################

###################################################
# Microphone array library
###################################################
add_library(pico_microphone_array_i2s INTERFACE)

target_sources(pico_microphone_array_i2s INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/microphone_array_i2s.c
)

target_include_directories(pico_microphone_array_i2s INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/include
)

pico_generate_pio_header(pico_microphone_array_i2s ${CMAKE_CURRENT_LIST_DIR}/src/microphone_array_i2s.pio)

target_link_libraries(pico_microphone_array_i2s INTERFACE pico_stdlib hardware_dma hardware_pio)

###################################################

###################################################
# SK9822 library
###################################################
add_library(pico_9822 INTERFACE)

target_sources(pico_9822 
    INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/sk9822.c
)

target_include_directories(pico_9822 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/include
)

pico_generate_pio_header(pico_9822 ${CMAKE_CURRENT_LIST_DIR}/src/sk9822.pio)

target_link_libraries(pico_9822 INTERFACE pico_stdlib hardware_dma hardware_pio)
###################################################

###################################################
# SK9822 animation
###################################################
add_library(pico_9822_led_animation INTERFACE)

target_sources(pico_9822_led_animation 
    INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/sk9822_led_animation.c
)

target_include_directories(pico_9822_led_animation INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/include
)

target_link_libraries(pico_9822_led_animation INTERFACE pico_stdlib pico_9822)
###################################################

# CURSOR NOTE: Original denisgav examples disabled — they use TinyUSB API from
# SDK <2.0.0 (tusb_init with arguments) which is incompatible with TinyUSB 0.16+
# bundled in SDK 2.1.0. Migrating those examples is out of scope for this project.
# Build the USB probe alongside the microphone target for bring-up/debugging.
# add_subdirectory("examples/hello_microphone_array")
# add_subdirectory("examples/usb_microphone_array")
# add_subdirectory("examples/sk9822")
add_subdirectory("examples/usb_probe")
add_subdirectory("examples/usb_microphone_array_6ch")
add_subdirectory("examples/usb_microphone_array_led")


###################################################
# Experimental embedded MVDR port (user-approved SCOPE-7 exception)
###################################################
option(SONITUDE_BUILD_EXPERIMENTAL_MVDR
    "Build the default-off RP2350/CMSIS-DSP MVDR scaffold" OFF)
option(SONITUDE_DSP_FAST_MATH
    "Enable -ffast-math only for the experimental DSP target" OFF)

if(SONITUDE_BUILD_EXPERIMENTAL_MVDR)
    add_subdirectory("examples/experimental_mvdr")
endif()
