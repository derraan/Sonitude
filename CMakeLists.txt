cmake_minimum_required(VERSION 3.20)

project(
  sonitude
  VERSION 0.1.0
  DESCRIPTION "Sonitude real-time audio proof-of-concept scaffold"
  LANGUAGES CXX
)

option(SONITUDE_BUILD_TESTS "Build Sonitude unit tests" ON)
option(SONITUDE_FETCH_DEPS "Fetch dependencies when not found locally" ON)

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
  src/app/logging.cpp
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

add_executable(
  sonitude_realtime
  src/main.cpp
)

target_link_libraries(
  sonitude_realtime
  PRIVATE
    sonitude_core
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

add_executable(
  sonitude_calibration_capture
  src/tools/calibration_capture.cpp
)

add_executable(
  sonitude_latency_marker
  src/tools/latency_marker.cpp
)

add_executable(
  sonitude_wav_replay
  src/tools/wav_replay.cpp
)

if(SONITUDE_BUILD_TESTS)
  enable_testing()

  add_executable(
    sonitude_unit_tests
    tests/unit/config_tests.cpp
  )

  target_link_libraries(
    sonitude_unit_tests
    PRIVATE
      sonitude_core
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
