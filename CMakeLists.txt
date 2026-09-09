cmake_minimum_required(VERSION 3.25)

project(
  sonitude
  VERSION 0.1.0
  DESCRIPTION "Sonitude real-time audio proof-of-concept scaffold"
  LANGUAGES CXX
)

option(SONITUDE_BUILD_TESTS "Build Sonitude unit tests" ON)
option(SONITUDE_FETCH_DEPS "Fetch dependencies when not found locally" ON)
option(SONITUDE_WITH_LIBSAMPLERATE "Enable libsamplerate-backed resampler" ON)
option(SONITUDE_ENABLE_SANITIZERS "Enable address/UB sanitizers" OFF)
option(SONITUDE_BUILD_FUZZERS "Build Sonitude libFuzzer harnesses" OFF)
set(SONITUDE_SANITIZER "none" CACHE STRING "Sanitizer mode: none,address,thread")
set_property(CACHE SONITUDE_SANITIZER PROPERTY STRINGS none address thread)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  option(SONITUDE_WITH_ALSA "Enable ALSA audio modules" ON)
else()
  option(SONITUDE_WITH_ALSA "Enable ALSA audio modules" OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

add_library(
  sonitude_core
  src/app/config.cpp
  src/app/calibration_config.cpp
  src/app/calibration_estimator.cpp
  src/app/calibration_writer.cpp
  src/app/logging.cpp
  src/audio/wav_io.cpp
  src/control/control_loop.cpp
  src/control/conversation_state_machine.cpp
  src/control/zones.cpp
  src/dsp/beamformer.cpp
  src/dsp/steering_lut.cpp
  src/dsp/array_profile.cpp
  src/dsp/fixed_binaural_mvdr.cpp
  src/dsp/binaural_renderer.cpp
  src/dsp/biquad_cascade.cpp
  src/dsp/calibration_applier.cpp
  src/dsp/hrtf_table.cpp
  src/dsp/limiter.cpp
  src/dsp/resampler_linear.cpp
  src/dsp/resampler_cubic.cpp
  src/dsp/resampler_src.cpp
  src/dsp/fft_radix2.cpp
  src/dsp/streaming_stft.cpp
  src/dsp/spectral_postfilter.cpp
  src/dsp/suppression_stage.cpp
  src/dsp/suppressor.cpp
  src/rt/rt_thread.cpp
  src/rt/wake_event.cpp
  src/spatial/mock_doa_provider.cpp
  src/spatial/odas_message_parser.cpp
  src/spatial/odas_provider.cpp
  src/spatial/source_tracker.cpp
)

target_include_directories(
  sonitude_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(
  sonitude_core
  PUBLIC
    yaml-cpp::yaml-cpp
    spdlog::spdlog
)

if(SONITUDE_WITH_LIBSAMPLERATE AND SONITUDE_LIBSAMPLERATE_FOUND)
  target_link_libraries(
    sonitude_core
    PUBLIC
      ${SONITUDE_LIBSAMPLERATE_TARGET}
  )
  target_compile_definitions(
    sonitude_core
    PUBLIC
      SONITUDE_WITH_LIBSAMPLERATE_ENABLED=1
      SONITUDE_RESAMPLER_BACKEND="libsamplerate"
  )
  message(STATUS "libsamplerate backend enabled")
else()
  target_compile_definitions(
    sonitude_core
    PUBLIC
      SONITUDE_WITH_LIBSAMPLERATE_ENABLED=0
      SONITUDE_RESAMPLER_BACKEND="linear"
  )
  if(SONITUDE_WITH_LIBSAMPLERATE)
    message(STATUS "libsamplerate not found; using linear fallback backend")
  endif()
endif()

if(SONITUDE_WITH_ALSA)
  add_library(
    sonitude_audio_alsa
    src/audio/alsa/alsa_probe.cpp
    src/audio/alsa/alsa_device.cpp
    src/audio/alsa/capture_worker.cpp
    src/audio/alsa/playback_worker.cpp
  )
  target_include_directories(
    sonitude_audio_alsa
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )
  target_link_libraries(
    sonitude_audio_alsa
    PUBLIC
      ALSA::ALSA
      sonitude_core
  )
  target_compile_definitions(
    sonitude_audio_alsa
    PUBLIC
      SONITUDE_HAS_ALSA=1
  )
else()
  target_compile_definitions(
    sonitude_core
    PUBLIC
      SONITUDE_HAS_ALSA=0
  )
endif()

if(SONITUDE_WITH_ALSA)
  add_executable(
    sonitude_realtime
    src/main.cpp
  )
  target_link_libraries(
    sonitude_realtime
    PRIVATE
      sonitude_core
      sonitude_audio_alsa
  )
  target_compile_definitions(
    sonitude_realtime
    PRIVATE
      SONITUDE_VERSION="${PROJECT_VERSION}"
  )

  add_executable(
    sonitude_device_probe
    src/tools/device_probe.cpp
  )
  target_link_libraries(
    sonitude_device_probe
    PRIVATE
      sonitude_audio_alsa
      sonitude_core
  )
endif()

if(SONITUDE_WITH_ALSA)
  add_executable(
    sonitude_capture_check
    src/tools/capture_check.cpp
  )
  target_link_libraries(
    sonitude_capture_check
    PRIVATE
      sonitude_audio_alsa
      sonitude_core
  )

  add_executable(
    sonitude_playback_check
    src/tools/playback_check.cpp
  )
  target_link_libraries(
    sonitude_playback_check
    PRIVATE
      sonitude_audio_alsa
      sonitude_core
  )

  add_executable(
    sonitude_loopback_diag
    src/tools/loopback_diag.cpp
  )
  target_link_libraries(
    sonitude_loopback_diag
    PRIVATE
      sonitude_audio_alsa
      sonitude_core
  )
endif()

add_executable(
  sonitude_calibration_capture
  src/tools/calibration_capture.cpp
)
target_link_libraries(
  sonitude_calibration_capture
  PRIVATE
    sonitude_core
)

add_executable(
  sonitude_calibration_estimate
  src/tools/calibration_estimate.cpp
)
target_link_libraries(
  sonitude_calibration_estimate
  PRIVATE
    sonitude_core
)

add_executable(
  sonitude_latency_marker
  src/tools/latency_marker.cpp
)

add_executable(
  sonitude_wav_replay
  src/tools/wav_replay.cpp
)
target_link_libraries(
  sonitude_wav_replay
  PRIVATE
    sonitude_core
)

add_executable(
  sonitude_odas_config_gen
  src/tools/odas_config_gen.cpp
)
target_link_libraries(
  sonitude_odas_config_gen
  PRIVATE
    sonitude_core
)

add_executable(
  sonitude_stream_process
  src/tools/stream_process.cpp
)
target_link_libraries(
  sonitude_stream_process
  PRIVATE
    sonitude_core
)

add_executable(
  sonitude_binaural_bench
  src/tools/binaural_bench.cpp
)
target_link_libraries(
  sonitude_binaural_bench
  PRIVATE
    sonitude_core
)

add_executable(
  sonitude_mvdr_bench
  src/tools/mvdr_bench.cpp
)
target_link_libraries(
  sonitude_mvdr_bench
  PRIVATE
    sonitude_core
)

if(SONITUDE_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SONITUDE_BUILD_FUZZERS requires clang/clang++")
  endif()

  foreach(fuzz_target IN ITEMS odas_parser wav_io)
    add_executable(
      fuzz_${fuzz_target}
      tests/fuzz/${fuzz_target}_fuzz.cpp
    )
    target_link_libraries(
      fuzz_${fuzz_target}
      PRIVATE
        sonitude_core
    )
    target_compile_options(
      fuzz_${fuzz_target}
      PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(
      fuzz_${fuzz_target}
      PRIVATE
        -fsanitize=fuzzer,address,undefined
    )
  endforeach()
endif()

if(SONITUDE_BUILD_TESTS)
  enable_testing()

  add_executable(
    sonitude_unit_tests
    tests/unit/config_tests.cpp
    tests/unit/audio_support_tests.cpp
    tests/unit/rt_tests.cpp
    tests/unit/asrc_sim_tests.cpp
    tests/unit/calibration_tests.cpp
    tests/unit/steering_lut_tests.cpp
    tests/unit/beamformer_tests.cpp
    tests/unit/biquad_tests.cpp
    tests/unit/array_profile_tests.cpp
    tests/unit/fixed_mvdr_tests.cpp
    tests/unit/limiter_tests.cpp
    tests/unit/stereo_limiter_tests.cpp
    tests/unit/snapshot_tests.cpp
    tests/unit/binaural_tests.cpp
    tests/unit/suppressor_tests.cpp
    tests/unit/stft_tests.cpp
    tests/unit/spectral_postfilter_tests.cpp
    tests/support/alloc_counter.cpp
    tests/unit/odas_parser_tests.cpp
    tests/unit/control_loop_tests.cpp
    tests/unit/source_tracker_tests.cpp
    tests/unit/zone_tests.cpp
    tests/unit/state_machine_tests.cpp
  )

  target_link_libraries(
    sonitude_unit_tests
    PRIVATE
      sonitude_core
  )
  target_include_directories(
    sonitude_unit_tests
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}"
  )

  target_compile_definitions(
    sonitude_unit_tests
    PRIVATE
      SONITUDE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  if(SONITUDE_SANITIZER STREQUAL "thread")
    target_compile_definitions(
      sonitude_unit_tests
      PRIVATE
        SONITUDE_DISABLE_ALLOC_COUNTER_GLOBAL_NEW=1
    )
  endif()

  add_test(
    NAME sonitude_unit_tests
    COMMAND sonitude_unit_tests
  )
endif()

if(NOT MSVC)
  if(SONITUDE_SANITIZER STREQUAL "address")
    set(_sonitude_sanitize_compile_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(_sonitude_sanitize_link_flags -fsanitize=address,undefined)
  elseif(SONITUDE_SANITIZER STREQUAL "thread")
    set(_sonitude_sanitize_compile_flags -fsanitize=thread -fno-omit-frame-pointer)
    set(_sonitude_sanitize_link_flags -fsanitize=thread)
  elseif(SONITUDE_ENABLE_SANITIZERS)
    set(_sonitude_sanitize_compile_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(_sonitude_sanitize_link_flags -fsanitize=address,undefined)
  endif()
endif()

if(DEFINED _sonitude_sanitize_compile_flags)
  get_property(_sonitude_sanitizer_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_tgt IN LISTS _sonitude_sanitizer_targets)
    get_target_property(_type ${_tgt} TYPE)
    if(_type STREQUAL "EXECUTABLE" OR _type STREQUAL "STATIC_LIBRARY" OR _type STREQUAL "SHARED_LIBRARY")
      target_compile_options(${_tgt} PRIVATE ${_sonitude_sanitize_compile_flags})
      target_link_options(${_tgt} PRIVATE ${_sonitude_sanitize_link_flags})
    endif()
  endforeach()
endif()
