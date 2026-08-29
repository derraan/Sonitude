# Sonitude Binaural Renderer DSP Design Proposal

## 1. Purpose

This document specifies a portable binaural-rendering subsystem for Sonitude.

The subsystem converts the processed directional mono signal into stereo headphone output.

The implementation shall be part of the C++ DSP core.

It shall not depend on:

- PySide6;
- Python;
- sounddevice;
- desktop file I/O;
- operating-system audio APIs.

The design shall permit later deployment on constrained edge hardware.

---

# 2. Design Objective

The current processing concept produces a directional mono signal.

The required future processing path is:

```text
microphone array
      |
      v
Calibration
      |
      v
Beamformer
      |
      v
Suppression
      |
      v
directional mono
      |
      v
BinauralRenderer
    /       \
   v         v
 Left       Right
    \       /
     v     v
   Stereo Limiter
        |
        v
       DAC
        |
        v
    Headphones
```

The renderer shall create two headphone channels that represent the selected external source direction.

---

# 3. Terminology

Use the term:

```text
binaural rendering
```

for the DSP operation.

Do not use:

```text
stereo duplication
```

or:

```text
stereo downmix
```

as equivalent terms.

The current operation:

```text
L = mono
R = mono
```

is the reference/bypass condition.

It is not HRTF binaural rendering.

---

# 4. Functional Inputs

The renderer shall accept:

```text
mono audio block
sample rate
azimuth
elevation
renderer configuration
```

Future versions may also accept:

```text
distance
head orientation
source confidence
```

These are not required for the first implementation.

---

# 5. Functional Outputs

The renderer shall output:

```text
left audio block
right audio block
```

with the same number of frames as the input block unless a documented algorithmic latency requires internal buffering.

Any fixed algorithmic latency shall be reported.

---

# 6. Direction Source

The renderer and beamformer shall use one authoritative effective direction.

Recommended model:

```text
                  EffectiveDirection
                     /         \
                    /           \
                   v             v
             Beamformer   BinauralRenderer
```

Do not maintain independent unsynchronized azimuth states.

For recorded tests, the direction can come from the steering script.

For production operation, it shall come from the resolved steering/control state.

---

# 7. Renderer Backends

Implement one common API with multiple backends.

```text
BinauralRenderer
|
+-- MONO_REFERENCE
|
+-- ITD_ILD
|
+-- COMPACT_HRTF
|
+-- FULL_HRTF_REFERENCE
```

The last backend can be introduced after the portable interfaces exist.

---

# 8. Backend 0 — Mono Reference

Definition:

```text
L[n] = x[n]
R[n] = x[n]
```

Purpose:

- bypass/reference;
- regression testing;
- A/B listening;
- fallback behaviour.

This mode shall not be labelled binaural HRTF rendering in measurements.

---

# 9. Backend 1 — ITD/ILD

The first edge-capable renderer shall implement:

- interaural time difference;
- interaural level difference.

Conceptual model:

```text
                  +--> fractional delay L --> gain L --> LEFT
MONO -------------+
                  +--> fractional delay R --> gain R --> RIGHT
```

For input x[n]:

```text
L[n] = gL(direction) * delay(x, dL(direction))

R[n] = gR(direction) * delay(x, dR(direction))
```

The implementation shall support fractional delay.

The exact acoustic model and coefficient tables shall be documented.

Do not claim individualized HRTF performance from this backend.

---

# 10. Backend 2 — Compact HRTF

This is the target edge renderer.

Recommended conceptual structure:

```text
                         +--> delay L --> compact FIR L --> gain L --> LEFT
MONO --------------------+
                         +--> delay R --> compact FIR R --> gain R --> RIGHT
```

The compact renderer shall preserve as much useful directional information as practical under edge-device CPU, RAM and latency constraints.

Do not fix the final FIR length in this design document.

Candidate lengths shall be benchmarked.

Examples for evaluation can include:

```text
16 taps
32 taps
64 taps
```

These are test candidates, not requirements.

---

# 11. Backend 3 — Full HRTF Reference

A desktop/reference implementation should support higher-resolution HRIR convolution.

Its primary purpose is not embedded deployment.

Its purposes are:

- establish a reference;
- evaluate compact-renderer approximation;
- generate regression fixtures;
- support perceptual A/B tests.

The reference backend should eventually support standardized spatial-acoustic datasets.

SOFA/AES69-compatible HRTF data is preferred for interchange.

Do not make SOFA parsing a dependency of the minimal embedded renderer.

---

# 12. Portable DSP Interface

Recommended public API:

```cpp
enum class BinauralBackend
{
  MonoReference,
  ItdIld,
  CompactHrtf,
  FullHrtfReference
};

struct BinauralDirection
{
  float azimuth_deg;
  float elevation_deg;
};

struct BinauralConfig
{
  std::uint32_t sample_rate_hz;
  BinauralBackend backend;
};

class BinauralRenderer
{
public:
  explicit BinauralRenderer(const BinauralConfig& config);

  void Reset();

  void SetDirection(const BinauralDirection& direction);

  void ProcessBlock(
      std::span<const float> mono,
      std::span<float> left,
      std::span<float> right);
};
```

The exact API can be adapted to existing Sonitude coding conventions.

The architectural requirements are:

- block processing;
- no GUI dependency;
- no heap allocation in the processing path after initialization;
- deterministic state;
- explicit reset;
- explicit configuration;
- explicit direction state.

---

# 13. Embedded Portability

The DSP design shall permit later implementation using:

```text
float32
Q31
Q15
```

where appropriate.

The portable Sonitude interface shall not expose CMSIS-DSP types.

Use:

```text
Sonitude DSP abstraction
        |
        +--> portable/reference C++ implementation
        |
        +--> CMSIS-DSP optimized implementation
```

This prevents desktop validation code from becoming dependent on one MCU library.

---

# 14. Memory Requirements

The renderer shall avoid dynamic memory allocation during realtime processing.

Memory shall be allocated or provided during initialization.

Track separately:

```text
coefficient memory
delay-line memory
filter-state memory
temporary block memory
```

The benchmark shall report memory use for each backend.

---

# 15. CPU Requirements

Benchmark each candidate backend using:

```text
processing time per block
processing time per sample
percentage realtime budget
```

The acceptance criterion shall be based on measured target hardware.

Do not claim Pico, STM32 or other MCU suitability from desktop timing alone.

---

# 16. Direction Changes

An instantaneous filter-state change can create an audible discontinuity.

The renderer shall implement a transition mechanism.

Candidate mechanisms include:

```text
coefficient interpolation

or

dual-render crossfade
```

The implementation shall be tested for discontinuity.

Do not select transition duration solely by assumption.

Make the transition duration configurable during validation.

---

# 17. Coordinate Convention

Define one coordinate convention.

The renderer shall document:

```text
0 degree azimuth
positive azimuth direction
negative azimuth direction
elevation sign
azimuth wrap range
```

The same convention shall be used by:

```text
DoA
steering
beamformer
test bench
binaural renderer
```

Add tests for:

```text
0 degrees
+90 degrees
-90 degrees
+179 degrees
-179 degrees
```

---

# 18. Left/Right Symmetry Tests

For a symmetric generic model:

```text
render(+theta)
```

shall correspond to the channel-mirrored equivalent of:

```text
render(-theta)
```

within the expected numerical tolerance.

This shall be a unit-test property.

---

# 19. Center Test

At:

```text
azimuth = 0
elevation = 0
```

the generic symmetric renderer should produce the expected left/right symmetry.

Do not assume bit-identical channels if the selected HRTF dataset does not have that property.

The test shall match the backend's defined model.

---

# 20. Impulse-Response Test

Feed:

```text
[1, 0, 0, 0, ...]
```

into the renderer.

Verify that each output corresponds to the expected:

```text
delay
gain
filter response
```

This shall be the primary deterministic filter-routing test.

---

# 21. Block-Boundary Test

Process one signal:

```text
as one complete block
```

and:

```text
as multiple sequential blocks
```

The outputs shall match within the expected numerical tolerance.

This verifies filter and delay state continuity.

---

# 22. Reset Test

After processing arbitrary audio:

```text
Reset()
```

shall return internal state to the documented initial state.

Processing a known fixture after reset shall match processing from a new renderer instance.

---

# 23. Non-Finite Input

Define behaviour for:

```text
NaN
+Inf
-Inf
```

Do not permit undefined propagation into the headphone output without an explicit policy.

The preferred behaviour is to sanitize or fail safely at an appropriate pipeline boundary.

The exact ownership of sanitization shall be documented.

---

# 24. Output Safety

Binaural filtering can change output peaks.

Therefore a limiter that operates only before the binaural renderer is not sufficient evidence of final stereo peak safety.

Recommended final order:

```text
Suppression
    |
    v
BinauralRenderer
    |
    v
Stereo Safety Limiter
    |
    v
DAC
```

If the existing limiter is mono-only, either:

- adapt it for independent/linked stereo operation; or
- introduce a final stereo safety stage.

The stereo limiting policy shall avoid unnecessary left/right image movement.

---

# 25. HRTF Data Architecture

Do not compile arbitrary SOFA parsing into the embedded realtime path.

Use an offline preparation step:

```text
SOFA/HRTF dataset
       |
       v
coefficient preparation tool
       |
       +--> reference HRIR data
       |
       +--> compact edge coefficient table
```

The embedded runtime shall consume a compact deterministic representation.

Store provenance:

```text
source HRTF dataset
subject/profile
sample rate
conversion version
compression/reduction method
coefficient checksum/version
```

---

# 26. HRTF Personalization

The first implementation may use a generic HRTF.

Do not claim personalized spatial accuracy.

The architecture should permit a future profile identifier:

```text
generic
profile_A
profile_B
...
```

Personalization is outside the first implementation scope.

---

# 27. Head Tracking

Head tracking is not required for the first renderer implementation.

However, the direction interface shall not prevent future transformation from world-relative source direction to head-relative source direction.

Future model:

```text
world source direction
        +
head orientation
        |
        v
head-relative direction
        |
        v
BinauralRenderer
```

---

# 28. Integration into sonitude_core

Add:

```text
src/dsp/binaural_renderer.hpp
src/dsp/binaural_renderer.cpp
```

and associated tests.

Recommended pipeline integration:

```text
CalibrationApplier
       |
DelaySumBeamformer
       |
directivity/omni blend
       |
ConservativeSuppressor
       |
BinauralRenderer
       |
Stereo Safety Limiter
```

The renderer shall be available to:

```text
sonitude_wav_replay
sonitude_stream_process
```

without requiring Python.

---

# 29. Diagnostic Outputs

Recorded replay should optionally expose:

```text
pre-binaural mono
post-binaural stereo
post-limiter stereo
```

Live processing should at minimum report:

```text
resolved renderer backend
effective direction
renderer processing time
```

Additional diagnostic taps shall be optional to avoid unnecessary realtime bandwidth.

---

# 30. Configuration

Suggested configuration shape:

```yaml
binaural:
  enabled: false
  backend: mono_reference

  direction:
    follow_steering: true

  transition:
    duration_ms: TBD_FROM_MEASUREMENT

  profile:
    id: generic
```

Compact-HRTF-specific parameters can be added only when their semantics are established.

Do not place guessed FIR lengths or performance claims in production defaults.

---

# 31. Benchmark Matrix

At minimum benchmark:

```text
Backend:
    mono reference
    ITD/ILD
    compact HRTF candidates
    full reference

Sample rate:
    target production rates

Block size:
    target production block sizes
```

Record:

```text
CPU time
RAM
coefficient storage
algorithmic latency
peak output
numerical error against reference
```

For edge targets, run the benchmark on actual target hardware.

---

# 32. Reference Comparison

For compact-HRTF development:

```text
same mono fixture
same direction
       |
       +--> full HRTF reference
       |
       +--> compact HRTF
```

Compare objective error.

Do not treat objective waveform error alone as proof of perceptual equivalence.

Listening evaluation remains necessary for spatial attributes.

---

# 33. Required Unit Tests

Minimum automated test set:

```text
construction/config validation
mono-reference identity
center-direction behaviour
left/right mirror symmetry
azimuth wraparound
impulse response
block continuity
reset determinism
direction transition
bounded finite output
invalid configuration
```

When compact filters are introduced:

```text
coefficient-table integrity
reference-response error
```

shall also be tested.

---

# 34. Required Integration Tests

`sonitude_wav_replay`:

```text
renderer OFF
renderer ON
direction change
stereo output
diagnostic output
```

`sonitude_stream_process`:

```text
renderer OFF
renderer ON
direction control
sequence continuity
stereo output
process reset
```

---

# 35. Non-Goals

The first implementation shall not claim:

- personalized HRTF accuracy;
- full elevation localization performance;
- validated externalization;
- a fixed optimal FIR length;
- verified Pico 2W realtime performance;
- verified STM32 realtime performance;
- production head tracking.

These require measurement.

---

# 36. Implementation Sequence

## Phase A

Implement:

```text
BinauralRenderer interface
MonoReference backend
tests
pipeline integration
```

## Phase B

Implement:

```text
ITD/ILD backend
fractional delay
direction tables/model
transition handling
tests
```

## Phase C

Implement:

```text
full/reference HRIR path
reference fixtures
```

## Phase D

Derive and implement:

```text
compact HRTF candidates
```

Compare them against the reference.

## Phase E

Benchmark on candidate edge hardware.

Only after these measurements shall one compact configuration become the embedded production candidate.

---

# 37. Definition of Done

The binaural DSP foundation is complete when:

1. the renderer exists in `sonitude_core`;
2. it has no Python or GUI dependency;
3. mono-reference and at least one true spatial backend operate;
4. direction is synchronized with effective steering;
5. recorded and streaming C++ tools can exercise it;
6. direction transitions do not create unhandled discontinuities;
7. output passes through final stereo peak protection;
8. deterministic DSP tests pass;
9. benchmark instrumentation exists;
10. unsupported perceptual and edge-performance claims remain explicitly unverified.

# 38. Engineering Principle

The desktop implementation is the reference environment.

The edge implementation is an optimization target.

Do not reduce the reference implementation until the reduced implementation can be compared against it.