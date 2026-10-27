cmake_minimum_required(VERSION 3.13)

# Standalone build: `cmake -S pcm5101a-pico -B build` builds the library and
# the hello_pcm5101a example. When pulled in via add_subdirectory() from a
# firmware project, only the pcm5101a_i2s library target is defined.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PCM5101A_TOP_LEVEL ON)
else()
    set(PCM5101A_TOP_LEVEL OFF)
endif()

if(PCM5101A_TOP_LEVEL)
    if(NOT DEFINED PICO_BOARD)
        set(PICO_BOARD pico2_w)
    endif()
    include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)
    project(pcm5101a_pico C CXX ASM)
    set(CMAKE_C_STANDARD 11)
    pico_sdk_init()
endif()

add_library(pcm5101a_i2s INTERFACE)

target_sources(pcm5101a_i2s INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/pcm5101a_i2s.c
)

target_include_directories(pcm5101a_i2s INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/include
)

pico_generate_pio_header(pcm5101a_i2s ${CMAKE_CURRENT_LIST_DIR}/src/pcm5101a_i2s.pio)

target_link_libraries(pcm5101a_i2s INTERFACE
    pico_time
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_gpio
    hardware_clocks
)

if(PCM5101A_TOP_LEVEL)
    add_subdirectory(examples/hello_pcm5101a)
endif()
