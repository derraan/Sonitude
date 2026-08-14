cmake_minimum_required(VERSION 3.20)

project(
  sonitude
  VERSION 0.1.0
  DESCRIPTION "Sonitude real-time audio proof-of-concept scaffold"
  LANGUAGES CXX
)

option(SONITUDE_BUILD_TESTS "Build Sonitude unit tests" ON)
option(SONITUDE_FETCH_DEPS "Fetch dependencies when not found locally" ON)
option(SONITUDE_WITH_LIBSAMPLERATE "Enable libsamplerate-backed resampler" ON)
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
  src/app/calibration_writer.cpp
  src/app/logging.cpp
  src/audio/wav_io.cpp
  src/control/control_loop.cpp
  src/control/conversation_state_machine.cpp
  src/control/zones.cpp
  src/dsp/beamformer.cpp
  src/dsp/calibration_applier.cpp
  src/dsp/limiter.cpp
  src/dsp/resampler_linear.cpp
  src/dsp/resampler_cubic.cpp
  src/dsp/resampler_src.cpp
  src/dsp/selective_suppressor.cpp
  src/dsp/suppressor.cpp
  src/rt/rt_thread.cpp
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

add_executable(
  sonitude_realtime
  src/main.cpp
)

target_link_libraries(
  sonitude_realtime
  PRIVATE
    sonitude_core
)
if(SONITUDE_WITH_ALSA)
  target_link_libraries(
    sonitude_realtime
    PRIVATE
      sonitude_audio_alsa
  )
endif()

target_compile_definitions(
  sonitude_realtime
  PRIVATE
    SONITUDE_VERSION="${PROJECT_VERSION}"
)

add_executable(
  sonitude_device_probe
  src/tools/device_probe.cpp
)
if(SONITUDE_WITH_ALSA)
  target_link_libraries(
    sonitude_device_probe
    PRIVATE
      sonitude_audio_alsa
      sonitude_core
  )
else()
  target_link_libraries(
    sonitude_device_probe
    PRIVATE
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

if(SONITUDE_BUILD_TESTS)
  enable_testing()

  add_executable(
    sonitude_unit_tests
    tests/unit/config_tests.cpp
    tests/unit/audio_support_tests.cpp
    tests/unit/rt_tests.cpp
    tests/unit/asrc_sim_tests.cpp
    tests/unit/calibration_tests.cpp
    tests/unit/beamformer_tests.cpp
    tests/unit/limiter_tests.cpp
    tests/unit/snapshot_tests.cpp
    tests/unit/suppressor_tests.cpp
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

  add_test(
    NAME sonitude_unit_tests
    COMMAND sonitude_unit_tests
  )
endif()
