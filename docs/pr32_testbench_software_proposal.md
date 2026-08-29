# PR #32 — Audio Algorithm Test Bench Software Development Proposal

**Status (2026-08-29, `feature/pyside6-testbench`):** the four work packages
below are implemented on PR #32. Treat this file as the original
requirements record, not a to-do list. Current behaviour, including
hour-scale streaming batch and live steering while playing, is documented
in `testbench/README.md` and `docs/binaural_renderer.md`.

Python still does not implement production DSP. `sonitude_realtime`
`--mode beamform` still duplicates mono to both ears.

## 1. Purpose

This document specifies the next development stage of the Sonitude PySide6 Audio Algorithm Test Bench introduced in PR #32.

The test bench shall remain an engineering validation interface for the Sonitude C++ DSP implementation.

Python shall not implement production DSP algorithms.

The application shall:

- load test audio;
- configure DSP tests;
- send control data to the C++ processing tools;
- receive processed audio and diagnostic data;
- display measurements;
- compare DSP stages;
- save reproducible test results.

The C++ DSP implementation shall remain the authoritative signal-processing implementation.

---

# 2. Scope

The next development stage contains four work packages:

1. multiformat audio input and output;
2. binaural-renderer test-bench integration;
3. DSP inspection and experiment functions;
4. reliability and correctness fixes.

Binaural signal processing itself is outside the scope of this document.

The binaural DSP implementation is specified in `docs/binaural_renderer_dsp_proposal.md`.

---

# 3. Current Architecture

The current test bench uses two C++ executables.

## 3.1 Recorded mode

```text
Audio file
    |
    v
Python test bench
    |
    v
sonitude_wav_replay
    |
    v
sonitude_core
    |
    +--> beamformed output
    +--> suppressed output
    +--> final processed output
```

## 3.2 Live mode

```text
Microphone device
      |
      v
sounddevice
      |
      v
Python RealtimeWorker
      |
      | framed binary IPC
      v
sonitude_stream_process
      |
      v
sonitude_core
      |
      | processed stereo PCM
      v
Python
      |
      v
audio output device
```

This process boundary shall remain.

Qt signals shall be used for communication between GUI and Python worker objects.

The binary protocol shall remain the communication mechanism between Python and the separate C++ process.

Files shall not be used as the live control transport.

---

# 4. Work Package A — General Audio File I/O

## 4.1 Requirement

The application shall accept:

- WAV;
- FLAC;
- MP3.

The loader shall preserve the source channel count.

The loader shall not automatically convert a multichannel source to mono or stereo.

The DSP input validator shall independently determine whether the decoded audio has the required Sonitude microphone channels.

For the present six-microphone configuration:

```text
6-channel FLAC
    -> decode
    -> valid candidate DSP input

6-channel WAV
    -> decode
    -> valid candidate DSP input

6-channel MP3
    -> decode
    -> valid candidate DSP input

2-channel MP3
    -> decode successfully
    -> reject as incompatible microphone-array input
```

A codec error and a channel-layout error shall be different errors.

---

## 4.2 Python Audio Abstraction

Replace the WAV-specific application abstraction with a format-independent abstraction.

Recommended change:

```text
testbench/app/audio_io/wav_loader.py
                  |
                  v
testbench/app/audio_io/audio_loader.py
```

Recommended interface:

```python
probe_audio(path) -> AudioMetadata

load_audio(path) -> AudioData

validate_dsp_input(audio, configuration) -> ValidationResult
```

`AudioMetadata` should contain at least:

```text
path
container
codec/subtype
sample_rate_hz
channels
frames
duration_s
source_bit_depth where available
```

Decoded processing samples shall use one canonical representation:

```text
float32 PCM
shape = [frames, channels]
```

The original channel order shall be preserved.

---

# 5. MP3 Support

The application shall use the existing Python audio I/O layer where possible.

The implementation shall verify codec capability at runtime.

Failure to decode MP3 shall produce a codec-capability error.

The application shall not silently invoke an external transcoder unless that behaviour is explicitly implemented and documented.

If a fallback decoder is later added, the result metadata shall record which decoder was used.

---

# 6. FLAC Output

The application shall support:

```text
WAV
FLAC
```

for final processed exports.

MP3 output is not required.

FLAC shall be treated as a lossless storage option.

The preferred architecture is:

```text
C++ DSP
   |
   v
canonical PCM result
   |
   v
Python result writer
   |
   +--> WAV
   |
   +--> FLAC
```

Changing the output container shall not change DSP execution.

Intermediate engineering artifacts can remain WAV during the first implementation.

Examples include:

```text
beamformed.wav
suppressed.wav
residual_beamform.wav
residual_limiter.wav
```

The final user-selected output can be:

```text
processed.wav

or

processed.flac
```

---

# 7. Work Package B — Binaural Renderer Integration

The test bench shall expose the C++ binaural renderer.

The test bench shall not implement the production binaural algorithm in Python.

The GUI shall support:

```text
Binaural Rendering
[ ] Enabled

Renderer:
[Mono Reference]
[ITD/ILD]
[Compact HRTF]
[Full HRTF Reference]     future/reference mode

Direction:
Azimuth
Elevation

[ ] Follow beamformer steering
```

Unavailable DSP backends shall be disabled or clearly marked unavailable.

A placeholder shall not be presented as a working renderer.

---

# 8. DSP Stage Inspection

The application shall evolve from final-output playback into a DSP inspection system.

The following stages should be available where the C++ backend exposes them:

```text
Input
Calibrated
Beamformed
Suppressed
Binaural Left/Right
Final Limited Left/Right
```

The GUI shall clearly distinguish:

```text
production DSP output

from

listening-only preview
```

The existing ear-cup stereo preview shall remain a listening convenience only.

It shall not be renamed or treated as binaural rendering.

---

# 9. A/B Comparison

The application should support rapid comparison between:

```text
A = input/raw preview
B = final processing
```

and:

```text
A = mono duplication
B = binaural renderer
```

and eventually:

```text
A = full HRTF reference
B = compact edge renderer
```

The application shall preserve gain context when an A/B comparison is intended to evaluate algorithmic differences.

Any loudness normalization used for listening shall be explicitly indicated and shall not modify measurement inputs.

---

# 10. Experiment Model

Each test run shall become a reproducible experiment.

Recommended result structure:

```text
test_0042/
|
+-- input_metadata.json
+-- runtime_config.yaml
+-- steering.csv
+-- processing_metadata.json
+-- metrics.json
|
+-- input_preview.wav
+-- beamformed.wav
+-- suppressed.wav
+-- binaural.wav
+-- processed.flac
|
+-- runtime_telemetry.json
```

The exact filenames can differ.

The important requirement is that the processing configuration and result remain associated.

---

# 11. Processing Metadata

Store at least:

```text
testbench version
Sonitude commit SHA
DSP executable version
protocol version

input filename
input format
sample rate
channel count
active channel map

geometry
calibration

steering trajectory

suppression:
    requested state
    resolved state

binaural:
    requested state
    resolved backend
    HRTF/profile identifier if applicable

limiter:
    requested state
    resolved state

output format
```

Requested configuration and resolved runtime configuration shall not be conflated.

---

# 12. Work Package C — Live Runtime Telemetry

The live interface shall display enough information to determine whether the test is valid.

Minimum telemetry:

```text
input block sequence
output block sequence

queue depth
dropped input blocks
protocol errors

DSP processing time
worker processing time

stream status
DSP subprocess status
```

Useful later telemetry includes:

```text
effective steering direction
suppressor activity/gain
limiter gain reduction
binaural backend
binaural transition state
```

---

# 13. Realtime Queue Fix

The current large FIFO design is unsuitable for interactive validation because it can preserve old audio while dropping new audio.

The live queue shall become a small bounded queue.

Recommended initial design:

```text
capacity = 2 or 3 blocks
```

This number is not an acceptance constant.

It shall be benchmarked.

When the queue is full:

```text
DROP OLDEST
KEEP NEWEST
```

The system shall count every dropped block.

The GUI shall display the count.

The application shall not silently operate on a large stale backlog.

---

# 14. Binary Protocol Revision

Introduce an explicit protocol version.

Recommended logical header:

```text
magic
protocol_version
message_type
sequence_number
frame_count
flags
payload_length
```

Possible message types:

```text
AUDIO_BLOCK
SET_CONFIG
RESET
TELEMETRY
ERROR
SHUTDOWN
```

Do not implement message types that have no current use only to satisfy this list.

The protocol shall reject:

```text
incorrect magic
unsupported protocol version
invalid frame count
invalid payload length
unknown required message type
truncated payload
non-finite control values
```

---

# 15. Sequence Tracking

Every audio request shall contain a sequence number.

The corresponding response shall contain the same sequence number.

Example:

```text
Python -> C++    sequence 12041
C++    -> Python sequence 12041
```

The worker shall detect:

```text
missing sequence
duplicate sequence
unexpected sequence
stale response
```

These conditions shall be visible to the test bench.

---

# 16. Worker Error Recovery

The realtime worker shall distinguish:

```text
audio-device failure
DSP subprocess failure
protocol failure
configuration failure
queue overrun
```

The UI shall not freeze when the DSP process exits.

The application shall support controlled stop and restart of the DSP subprocess.

Unexpected exceptions in the worker shall be reported to the GUI.

---

# 17. Steering Error Fix

Steering-angle error shall use circular angular distance.

Do not use:

```text
measured - expected
```

directly across the ±180 degree boundary.

Use the equivalent of:

```text
((measured - expected + 180) % 360) - 180
```

Add boundary tests such as:

```text
expected = +179
measured = -179

absolute error = 2 degrees
```

---

# 18. Directivity-Control Naming Fix

The current `width` implementation does not demonstrate physical beamwidth.

It blends the directional beamformer result toward a six-microphone average.

Do not label this control as measured beam width.

Use a name that describes the implemented operation.

Recommended UI wording:

```text
Directional / Omni Blend
```

Keep internal compatibility aliases only where necessary.

Do not claim HPBW or physical cone width without measured array-response evidence.

---

# 19. Suppression-State Fix

The test metadata shall distinguish:

```text
requested suppression state

from

resolved C++ suppression state
```

A command-line flag and YAML configuration shall not produce ambiguous experiment metadata.

Prefer an explicit resolved model such as:

```text
AUTO
ON
OFF
```

if compatible with the current runtime architecture.

---

# 20. Metric Corrections

## 20.1 Intelligibility Proxy

The current approximate-SII metric shall not be used as an acceptance metric.

The existing implementation can return a plausible intermediate score for a noise-only negative control.

Until replaced or validated:

```text
intelligibility_proxy
```

shall be clearly marked:

```text
EXPERIMENTAL
NOT ANSI/ASA S3.5 SII
NOT ACCEPTANCE-GATING
```

It may also be removed from the default metrics panel.

## 20.2 SNR

Do not call total mixture-power divided by reference-noise power strict speech SNR.

Rename the metric or change the required inputs so that the definition matches the calculation.

Add negative controls.

---

# 21. CI Requirements

The current C++ build check is insufficient as the only integration gate.

CI shall run:

```text
CMake configure
C++ build
CTest

Python dependency installation
pytest

real sonitude_wav_replay integration
real sonitude_stream_process integration
```

A test that silently skips because the C++ executable is unavailable shall not satisfy the integration gate.

The CI environment shall provide the executable paths explicitly.

---

# 22. Required Audio I/O Tests

Add fixtures for:

```text
6-channel WAV
6-channel FLAC
6-channel MP3 where supported

2-channel WAV
2-channel FLAC
2-channel MP3

malformed file
empty file
unsupported format
NaN/Inf decoded data where constructible
```

Tests shall verify:

```text
channel order preservation
sample rate preservation
channel count preservation
duration
metadata
expected rejection reason
FLAC export/read-back
```

For lossless WAV/FLAC conversion, compare decoded PCM within the tolerance appropriate to the chosen source/output sample representation.

---

# 23. Required Realtime Tests

Add tests for:

```text
queue overflow
drop-oldest behaviour
sequence-number matching
sequence gap
duplicate response
invalid magic
invalid protocol version
truncated payload
DSP process termination
worker restart
malformed control state
```

Tests shall verify the actual rule.

Do not accept proxy assertions that merely correlate with the expected behaviour on one fixture.

---

# 24. Binaural Test-Bench Tests

The application integration tests shall verify:

```text
renderer disabled
renderer enabled
backend selection
beamformer-direction following
manual direction
configuration persistence
binaural stereo output
output metadata
FLAC binaural export
```

DSP correctness itself belongs to the DSP unit tests.

---

# 25. Non-Goals

This work shall not:

- implement production DSP in Python;
- use CSV file watching for realtime control;
- add MP3 output;
- silently downmix incompatible input;
- claim the directional/omni blend is physical beamwidth;
- claim the experimental intelligibility proxy is standardized SII;
- claim the test bench represents complete production-runtime end-to-end behaviour.

---

# 26. Definition of Done

This work package is complete when:

1. WAV, FLAC and MP3 inputs are supported as specified.
2. Multichannel channel order is preserved.
3. WAV and FLAC final outputs work.
4. Binaural DSP can be controlled and inspected from the application.
5. Live queue overruns are bounded and visible.
6. Binary IPC is versioned and sequence-tracked.
7. DSP process failures do not freeze the UI.
8. Steering wraparound is correct.
9. directivity/omni blend terminology matches implementation.
10. suppression metadata records resolved behaviour.
11. invalid metrics are removed from acceptance decisions.
12. Python and real Python/C++ integration tests run in CI.
13. saved experiments contain sufficient provenance to reproduce the processing configuration.
14. documentation describes the difference between test-bench preview functions and production DSP.

# 27. Acceptance Principle

A green GUI is not evidence that the DSP is correct.

The test bench is accepted only when its controls, outputs, metadata and tests make the actual C++ behaviour observable and reproducible.