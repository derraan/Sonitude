include(FetchContent)

set(SONITUDE_LIBSAMPLERATE_FOUND OFF)
set(SONITUDE_LIBSAMPLERATE_TARGET "")

if(SONITUDE_WITH_ALSA)
  find_package(ALSA REQUIRED)
endif()

find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
  if(NOT SONITUDE_FETCH_DEPS)
    message(FATAL_ERROR "yaml-cpp not found and SONITUDE_FETCH_DEPS=OFF")
  endif()
  FetchContent_Declare(
    yaml_cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
  )
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(yaml_cpp)
endif()

find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  if(NOT SONITUDE_FETCH_DEPS)
    message(FATAL_ERROR "spdlog not found and SONITUDE_FETCH_DEPS=OFF")
  endif()
  FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
  )
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spdlog)
endif()

if(SONITUDE_WITH_LIBSAMPLERATE)
  find_package(Samplerate QUIET)
  if(Samplerate_FOUND)
    set(SONITUDE_LIBSAMPLERATE_FOUND ON)
    set(SONITUDE_LIBSAMPLERATE_TARGET Samplerate::samplerate)
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(SAMPLERATE QUIET samplerate)
      if(SAMPLERATE_FOUND)
        add_library(sonitude_samplerate_external INTERFACE)
        target_include_directories(sonitude_samplerate_external INTERFACE ${SAMPLERATE_INCLUDE_DIRS})
        target_link_libraries(sonitude_samplerate_external INTERFACE ${SAMPLERATE_LINK_LIBRARIES})
        set(SONITUDE_LIBSAMPLERATE_FOUND ON)
        set(SONITUDE_LIBSAMPLERATE_TARGET sonitude_samplerate_external)
      endif()
    endif()
  endif()
endif()
