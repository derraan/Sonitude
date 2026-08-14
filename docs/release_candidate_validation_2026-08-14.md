# Sonitude release-candidate validation — 2026-08-14

## Scope and provenance

- Integration branch: `integration/release-candidate-2026-08-14`
- Base: `origin/main` at `ecac64f62813901ea6cbb7c1a634817151877968`
- Validated source-tree commit: `0b6ed23e73b62ab0db6d9c87cfeec3daae3f005e`
- PR #27 foundation: `c16a005e1668d13643508b52306469c50873dd91`
- PR #24 semantic source: `f782795c2f016839595e0e0eb8109930a80b9e74`
- PR #25 tooling source: `c22b858df9a6101aa1123f5aca24057c76a91a5a`
- PR #26 quality-gate source: `3a81f1455d2a8346e6009be6fdb2261b75239d5c`
- PR #23 was not integrated. PR #27 supersedes it.

PR #27 was merged first. PRs #24, #25, and #26 were manually reconciled onto
that runtime so the `SteeringChannel<RtSteeringSnapshot>` handoff, explicit
audio-block ownership, and single lifecycle authority remained intact.

## Toolchains

Linux host verification ran under WSL2 Ubuntu 24.04:

- CMake 3.28.3
- Ninja 1.11.1
- GCC/G++ 13.3.0
- Clang/clang-tidy/clang-format 18.1.3
- ALSA 1.2.11
- Pico SDK 2.1.0

The firmware gate ran on the Windows host with:

- CMake 3.29.2
- Ninja 1.12.0
- Arm GNU Toolchain 13.2.rel1 (`arm-none-eabi-gcc` 13.2.1)

Secret scanning used Gitleaks 8.30.1.

Clang 18 and clang-format 18 were extracted under the WSL user's local tool
directory because system installation required interactive `sudo`
authentication. Gitleaks was installed under the Windows user's local tool
directory. No repository dependency or system package database was changed.

## Host-gate results

- PASS — clean RelWithDebInfo build and all three CTest targets, ALSA OFF and
  libsamplerate OFF.
- PASS — clean RelWithDebInfo build and all three CTest targets, ALSA ON and
  libsamplerate ON.
- PASS — additional clean Release matrix for libsamplerate ON, libsamplerate
  OFF, and ALSA OFF. All builds were warning-free and all tests passed.
- PASS — Clang 18 ASan + UBSan full suite: 3/3 tests.
- PASS — Clang 18 TSan integrated concurrency/lifecycle suite: 1/1 test.
- PASS — clang-format 18 over 72 changed first-party C/C++ files.
- PASS — zero-matching-file format path without a formatter installed.
- PASS — clang-tidy 18 with CI checks on WAV and ODAS parsers.
- PASS — ODAS libFuzzer campaign: 369,159 executions in 61 seconds, no finding.
- PASS — WAV libFuzzer campaign: 1,385,767 executions in 61 seconds, no finding.
- PASS — Gitleaks full-history scan: 37 commits and approximately 1.79 MB
  scanned, no leaks.
- PASS — Pico 2W firmware: 276 build steps and all configured ELF outputs.
  Primary six-channel UF2 generated at 86,016 bytes.
- PASS — explicit synthetic calibration path.
- PASS — calibration rejection identified channel
  `M1_upper_inner_right`, measured correlation `0.00435808`, required
  threshold `0.1`, and did not create/replace output YAML.
- PASS — `config/production_pi.yaml` validation.
- PASS — `git diff --check`.
- PASS — GitHub Actions YAML parsed locally.

The integration test remains labelled `smoke`; it is not represented as a
hardware or full-system test.

## Validation commands

The final-tree host validation used the following commands. Build directories
were new or configured with CMake `--fresh`; no stale build output was used as
evidence.

```bash
cmake -S . -B build-final-off -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSONITUDE_FETCH_DEPS=OFF \
  -DSONITUDE_WITH_ALSA=OFF \
  -DSONITUDE_WITH_LIBSAMPLERATE=OFF
cmake --build build-final-off --parallel
ctest --test-dir build-final-off --output-on-failure --timeout 180

cmake -S . -B build-final-on -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSONITUDE_FETCH_DEPS=OFF \
  -DSONITUDE_WITH_ALSA=ON \
  -DSONITUDE_WITH_LIBSAMPLERATE=ON
cmake --build build-final-on --parallel
ctest --test-dir build-final-on --output-on-failure --timeout 180
```

The additional clean Release matrix used:

```bash
cmake -S . -B build-verify-release-on -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSONITUDE_FETCH_DEPS=OFF \
  -DSONITUDE_WITH_ALSA=ON -DSONITUDE_WITH_LIBSAMPLERATE=ON
cmake --build build-verify-release-on --parallel
ctest --test-dir build-verify-release-on --output-on-failure --timeout 180

cmake -S . -B build-verify-release-off -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSONITUDE_FETCH_DEPS=OFF \
  -DSONITUDE_WITH_ALSA=ON -DSONITUDE_WITH_LIBSAMPLERATE=OFF
cmake --build build-verify-release-off --parallel
ctest --test-dir build-verify-release-off --output-on-failure --timeout 180

cmake -S . -B build-verify-alsa-off -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSONITUDE_FETCH_DEPS=OFF \
  -DSONITUDE_WITH_ALSA=OFF -DSONITUDE_WITH_LIBSAMPLERATE=OFF
cmake --build build-verify-alsa-off --parallel
ctest --test-dir build-verify-alsa-off --output-on-failure --timeout 180
```

Sanitizer gates:

```bash
bash scripts/run_sanitizer_tests.sh address --fresh -G Ninja \
  -DCMAKE_CXX_COMPILER=/home/darren/.local/clang-18/usr/bin/clang++-18 \
  -DSONITUDE_FETCH_DEPS=OFF -DSONITUDE_WITH_LIBSAMPLERATE=ON

bash scripts/run_sanitizer_tests.sh thread --fresh -G Ninja \
  -DCMAKE_CXX_COMPILER=/home/darren/.local/clang-18/usr/bin/clang++-18 \
  -DSONITUDE_FETCH_DEPS=OFF -DSONITUDE_WITH_LIBSAMPLERATE=ON
```

Format and static-analysis gates:

```bash
GIT=git.exe \
CLANG_FORMAT=/home/darren/.local/clang-format-18/usr/bin/clang-format-18 \
python3 scripts/check_format.sh "$(git.exe merge-base HEAD origin/main)"

py -3 scripts/check_format.sh HEAD

cmake -S . -B build-verify-tidy -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/home/darren/.local/clang-18/usr/bin/clang++-18 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DSONITUDE_FETCH_DEPS=OFF \
  -DSONITUDE_WITH_ALSA=OFF \
  -DSONITUDE_WITH_LIBSAMPLERATE=ON

clang-tidy-18 -p build-verify-tidy \
  --checks=-*,clang-analyzer-*,bugprone-* \
  --warnings-as-errors=* \
  src/audio/wav_io.cpp src/spatial/odas_message_parser.cpp
```

Fuzz gates:

```bash
cmake -S . -B build-verify-fuzz -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/home/darren/.local/clang-18/usr/bin/clang++-18 \
  -DSONITUDE_BUILD_TESTS=OFF \
  -DSONITUDE_BUILD_FUZZERS=ON \
  -DSONITUDE_FETCH_DEPS=OFF \
  -DSONITUDE_WITH_ALSA=OFF \
  -DSONITUDE_WITH_LIBSAMPLERATE=ON
cmake --build build-verify-fuzz --target fuzz_odas_parser fuzz_wav_io --parallel
./build-verify-fuzz/fuzz_odas_parser \
  -max_total_time=60 -timeout=5 -rss_limit_mb=1024 \
  tests/fuzz/corpus/odas
./build-verify-fuzz/fuzz_wav_io \
  -max_total_time=60 -timeout=5 -rss_limit_mb=1024 \
  tests/fuzz/corpus/wav
```

Calibration and production-configuration checks:

```bash
./build-verify-alsa-off/sonitude_calibration_capture \
  --synthetic --seconds 1 \
  --output build-verify-alsa-off/rc_validation_capture.wav
./build-verify-alsa-off/sonitude_calibration_estimate \
  --synthetic \
  --input build-verify-alsa-off/rc_validation_capture.synthetic.wav \
  --output build-verify-alsa-off/rc_validation_estimate.yaml \
  --config config/default.yaml \
  --min-correlation 0
./build-verify-alsa-off/sonitude_calibration_estimate \
  --synthetic \
  --input build-verify-alsa-off/rc_validation_capture.synthetic.wav \
  --output build-verify-alsa-off/rc_validation_rejected.yaml \
  --config config/default.yaml \
  --min-correlation 0.1
./build-verify-release-on/sonitude_realtime \
  --config config/production_pi.yaml --validate-config
```

Firmware, secret, and repository-integrity gates:

```powershell
$env:PICO_SDK_PATH = "C:\Users\darre\.pico-sdk\sdk\2.1.0"
$env:PICO_TOOLCHAIN_PATH = "C:\Users\darre\.pico-sdk\toolchain\13_2_Rel1"
cmake -S . -B build-verify-firmware -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-verify-firmware --parallel

gitleaks.exe git --redact --no-banner .
py -3 -c "import pathlib, yaml; yaml.safe_load(pathlib.Path('.github/workflows/linux-build.yml').read_text())"
git diff --check
```

## Warnings and limitations

- This is Linux host evidence from WSL2, not Raspberry Pi evidence.
- No configured `hw:PicoMic,0` device was available in WSL. A production
  startup attempt therefore failed while opening ALSA capture, as expected.
  It does not prove memory locking, realtime scheduling, or device I/O.
- Synthetic calibration artifacts are explicitly labelled non-hardware
  evidence. The `0` threshold used on the passing synthetic path is not a
  production acceptance threshold.
- clang-tidy reported warnings only in non-user/system code and suppressed
  them; both targeted first-party sources passed with warnings-as-errors.
- GitHub CI URLs are pending until the integration branch and draft PR are
  pushed in Phase 9. Add the workflow and draft PR URLs here after those runs
  complete.

## GitHub CI

Pending branch publication. Required jobs are:

- Build and test: libsamplerate ON
- Build and test: libsamplerate OFF
- Build and test: ALSA OFF
- ASan + UBSan
- TSan integrated concurrency/lifecycle tests
- clang-format 18
- clang-tidy 18
- bounded ODAS and WAV fuzzing
- Pico firmware build
- Gitleaks

CI run URLs: **pending branch push and draft PR creation**.

## Hardware validation pending

No item in this section has been passed by WSL, synthetic input, or firmware
compilation. All require Raspberry Pi execution with the configured ALSA
devices and archived logs/artifacts:

- [ ] Run the production configuration with `require_realtime: true`.
- [ ] Prove mandatory `mlockall(MCL_CURRENT | MCL_FUTURE)` succeeds.
- [ ] Record the scheduling table: capture FIFO 80, playback FIFO 78,
      control/telemetry SCHED_OTHER, and no `DEGRADED` thread.
- [ ] Complete at least a one-hour passthrough soak.
- [ ] Complete at least a one-hour beamform soak.
- [ ] Record capture and playback xruns.
- [ ] Record pool exhaustion, starvation, and high-water values.
- [ ] Record capture deadline misses and measured maximum execution times.
- [ ] Verify event-driven ALSA capture wait.
- [ ] Deliver SIGINT while capture is blocked and verify clean teardown.
- [ ] Force playback failure and verify bounded shutdown.
- [ ] Exercise an unreachable ODAS endpoint and recovery/backoff.
- [ ] Exercise xrun recovery under contention.
- [ ] Exercise repeated process start/stop.
- [ ] Select the final project-approved correlation threshold through hardware
      characterization.
- [ ] Capture hardware calibration results and prove low-quality channels are
      rejected without replacing the accepted calibration.

## Current verdict

**HOST INTEGRATION GREEN — HARDWARE APPROVAL PENDING**
