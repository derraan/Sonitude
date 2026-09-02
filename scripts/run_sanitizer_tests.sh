#!/usr/bin/env bash
# Builds and runs the unit test suite under ThreadSanitizer and then under
# AddressSanitizer + UndefinedBehaviorSanitizer.
#
# These are non-realtime builds on purpose. TSan interposes on synchronisation
# primitives and inflates timing, so it is used to prove the absence of data
# races, never to judge realtime behaviour.
#
# ALSA is disabled because the sanitizer targets only need the core library and
# the test binary; the device paths are exercised on hardware instead.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

selection="${1:-all}"
if [[ "${selection}" == "thread" || "${selection}" == "address" || "${selection}" == "all" ]]; then
  if (($# > 0)); then
    shift
  fi
else
  echo "Usage: $0 [thread|address|all] [extra CMake arguments...]" >&2
  exit 2
fi
extra_cmake_args=("$@")

# ThreadSanitizer aborts with "unexpected memory mapping" when the kernel uses
# more ASLR entropy than its shadow mapping supports, which is the default on
# recent kernels. Prefer running with randomisation disabled when possible.
run_no_aslr() {
  if setarch --addr-no-randomize true >/dev/null 2>&1; then
    setarch --addr-no-randomize "$@"
  else
    "$@"
  fi
}

sanitizers=(thread address)
if [[ "${selection}" != "all" ]]; then
  sanitizers=("${selection}")
fi

for sanitizer in "${sanitizers[@]}"; do
  build_dir="build-${sanitizer}"
  echo "=== configuring ${sanitizer} ==="
  cmake -S . -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSONITUDE_SANITIZER="${sanitizer}" \
    -DSONITUDE_WITH_ALSA=OFF \
    "${extra_cmake_args[@]}"
  echo "=== building ${sanitizer} ==="
  if [[ "${sanitizer}" == "thread" ]]; then
    cmake --build "${build_dir}" --target sonitude_concurrency_tests -j "$(nproc)"
  else
    cmake --build "${build_dir}" -j "$(nproc)"
  fi
  echo "=== running ${sanitizer} ==="
  if [[ "${sanitizer}" == "thread" ]]; then
    TSAN_OPTIONS="halt_on_error=1" run_no_aslr \
      ctest --test-dir "${build_dir}" --output-on-failure -L concurrency
  else
    ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" \
      UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
      run_no_aslr ctest --test-dir "${build_dir}" --output-on-failure
  fi
  echo "=== ${sanitizer} passed ==="
done

echo "All sanitizer runs passed."
