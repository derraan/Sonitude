cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED CMSISDSP_ROOT)
    message(FATAL_ERROR
        "Set CMSISDSP_ROOT to an ARM-software/CMSIS-DSP checkout when "
        "SONITUDE_BUILD_EXPERIMENTAL_MVDR=ON")
endif()

if(NOT TARGET CMSISDSP)
    # CMSIS-DSP's upstream CMake expects CMSISCORE to contain Core/Include.
    set(CMSISCORE "${PICO_SDK_PATH}/src/rp2_common/cmsis/stub/CMSIS/Core"
        CACHE PATH "CMSIS-Core root used by CMSIS-DSP" FORCE)
    add_subdirectory("${CMSISDSP_ROOT}/Source" "${CMAKE_BINARY_DIR}/cmsis-dsp")
endif()

add_executable(experimental_mvdr_pico2w
    main.cpp
    embedded_mvdr.cpp
)

target_compile_features(experimental_mvdr_pico2w PRIVATE cxx_std_20)
target_compile_options(experimental_mvdr_pico2w PRIVATE -O3)
if(SONITUDE_DSP_FAST_MATH)
    target_compile_options(experimental_mvdr_pico2w PRIVATE -ffast-math)
endif()

target_link_libraries(experimental_mvdr_pico2w PRIVATE
    CMSISDSP::CMSISDSP
    cmsis_core
    hardware_clocks
    pico_microphone_array_i2s
    pico_multicore
    pico_stdlib
)

pico_enable_stdio_usb(experimental_mvdr_pico2w 0)
pico_enable_stdio_uart(experimental_mvdr_pico2w 1)
pico_add_extra_outputs(experimental_mvdr_pico2w)
