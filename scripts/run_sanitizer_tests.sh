#!/usr/bin/env bash
# Run unit tests under ThreadSanitizer and/or Address+UB sanitizers.
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
  cmake --build "${build_dir}" --target sonitude_unit_tests -j "$(nproc)"

  echo "=== running ${sanitizer} ==="
  if [[ "${sanitizer}" == "thread" ]]; then
    TSAN_OPTIONS="halt_on_error=1" run_no_aslr \
      ctest --test-dir "${build_dir}" --output-on-failure -R sonitude_unit_tests
  else
    ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" \
      UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
      run_no_aslr ctest --test-dir "${build_dir}" --output-on-failure -R sonitude_unit_tests
  fi
done

echo "All sanitizer runs passed."
