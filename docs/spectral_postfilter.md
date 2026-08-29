# Experimental spectral postfilter (PR-scope prototype)

Status: **EXPERIMENTAL**. Disabled unless `suppression.backend: spectral` is selected
and suppression is enabled. This document describes what was implemented, not a
claim that the algorithm fits Pico 2 W, STM32H7, or the 6–10 ms M8 budget.

## Inspection note (verified before implementation)

| Item | Verified fact |
| --- | --- |
| PR #32 head | `77978344aa4f28e7c2a3ce08f625b30d351e7c35` on `feature/pyside6-testbench` |
| Worktree | `feature/pr32-spectral-postfilter` derived from that commit |
| Sample rate / block | Default capture 44100 Hz, `period_frames` 64. Tools accept other rates; DSP math uses the runtime rate. |
| Existing suppressor | Time-domain `ConservativeSuppressor`: full-band envelope + floor + fade. In-place. |
| Call sites | `src/main.cpp`, `src/tools/wav_replay.cpp`, `src/tools/stream_process.cpp` |
| Config | `runtime.suppression.{enabled,fade_ms,activity_threshold,confidence_threshold}` required; `backend` and `spectral.*` optional |
| FFT in repo | None. Host radix-2 added with unnormalized forward / 1/N inverse. CMSIS-DSP not linked. |
| Allocator tests | None prior; test-only `operator new` counter in `tests/support/alloc_counter.cpp` |
| Binaural / limiter | Spectral is mono, after beamformer/blend, before existing mono limiter and binaural. Stereo limiter still last when binaural is on. |
| XRUN into DSP | Capture XRUN skips the period (`readBlock` false). No concealment PCM is delivered. `setEstimatorHold` runs on the next successful period in `sonitude_realtime`. Stream protocol aborts on sequence gaps rather than concealing. |
| Click-free backend switch | Conservative already ramps per sample. Backend is selected at configure/prepare. Live mid-stream spectral↔off is unsupported (delay mismatch). |

Assumptions: six-channel map/order unchanged; Python does not reimplement DSP.

Unknowns / NOT MEASURED: Pico 2 W, STM32H7, end-to-end loopback latency, six-channel array health/geometry evidence.

## Signal flow

Pre-change (PR #32):

```text
6ch PCM -> map/cal -> delay-and-sum -> directional/omni blend (tools)
  -> ConservativeSuppressor if enabled else copy
  -> mono peak limiter
  -> optional binaural
  -> stereo limiter if binaural
```

Post-change:

```text
6ch PCM -> map/cal -> delay-and-sum -> directional/omni blend (tools)
  -> SuppressionStage: off | conservative | spectral (alternatives, not series)
  -> mono peak limiter (unchanged)
  -> optional binaural (unchanged)
  -> stereo limiter if binaural (unchanged)
```

Post-beamform (not per-mic) so inter-channel phase used by the beamformer is not
independently gain-modulated. Mono postfiltering is not direction or distance
separation.

## STFT

Periodic Hann `w[n] = 0.5 (1 - cos(2 π n / N))`, n = 0..N-1.

Forward DFT unnormalized; inverse scaled by 1/N (host radix-2; future CMSIS-DSP
adapters must match this contract).

Analysis and synthesis use the same window. Overlap-add is divided by the COLA
of `w²` at hop `N/4` (computed in `prepare`).

Supported pairs: **128/32**, **256/64**. **512/128 rejected**.

Calculated (not measured e2e) at 44.1 kHz for 128/32: analysis 2.902 ms, hop
0.726 ms, N−H lookahead 96 samples (2.177 ms).

Measured first-arrival delay (impulse peak): **127 samples** (128/32), **255
samples** (256/64). Reconstruction tests skip `delay + N` at the start and `N`
at the tail. Absolute error threshold in tests: `2e-4`.

## Algorithm actually implemented

Name: **AsymmetricNoisePowerTracker + BoundedWienerGain** (heuristic / inspired, not a
published algorithm identity). Previously labeled MinimaTrackedWienerPostfilter.

Clean-room original code. Not copied from SpeexDSP, RNNoise, or paper
reference implementations.

Not IMCRA, not MCRA, not OM-LSA, not Ephraim–Malah: those names require the
published equations and parameter meanings. This backend uses:

1. Periodogram `P[k] = Re{X[k]}² + Im{X[k]}²` for `k = 0..N/2`.
2. Smoothed power `S` with time constant 32 ms at the hop rate:
   `S ← a_s S + (1-a_s) P`, `a_s = exp(-T_hop / 0.032)`.
3. Noise `λ` is an asymmetric smoother, not a sliding minimum. On the first
   allowed update, every bin is initialized to the **cross-frequency median** of
   `P`, not to `P[k]`. Later hops snap `λ` down to `S` when `S < λ`, else rise
   slowly (`τ = 0.48 s`). Bins with `S > 6 · median(S)` are treated as tonal and
   are not absorbed into `λ`.
4. Learning is permitted only while focus is active, confidence is at/above the
   configured threshold, and the estimator is not held. Unfocused / below-threshold
   hops freeze `S` and `λ` completely (no first-frame init, no downward snap).
5. Heuristic a priori SNR
   `ξ = a_dd (G_prev² γ) + (1-a_dd) max(γ-1, 0)`
   with `γ = P / (2 λ)` (noise overestimate 2) and τ_dd = 48 ms. This is **not**
   the Ephraim–Malah decision-directed recurrence (no stored previous posterior).
6. Wiener `G = ξ/(1+ξ)`, clamp `[G_floor, 1]`, time-smoothed (τ = 16 ms),
   then 3-bin frequency smoother `[0.25, 0.5, 0.25]`. A bypass mix ramps toward
   the Wiener gain when focused and initialized, and toward unity otherwise.
7. No bin amplification. Hermitian bins share `G[k]`. Telemetry `currentGain()`
   is power-weighted across bins.

Consulted only to bound naming (not implemented): Cohen, IEEE Trans. Speech
Audio Process., 11(5):466–475, 2003 (IMCRA); Cohen & Berdugo, Signal
Processing, 81:2403–2418, 2001; Gannot & Cohen, IEEE Trans. Speech Audio
Process., 12(6):561–571, 2004 (TF-GSC postfilter — out of scope). Arm
CMSIS-DSP FFT docs were not used in code; the replacement path is the
`Radix2Fft` scaling contract in `fft_backend.hpp`.

## Configuration

| Key | Type | Units | Default | Range |
| --- | --- | --- | --- | --- |
| `suppression.enabled` | bool | — | false | — |
| `suppression.backend` | string | — | `conservative` if omitted | `off` \| `conservative` \| `spectral` |
| `suppression.fade_ms` | float | ms | 120 | [1, 1000] (conservative only) |
| `suppression.activity_threshold` | float | linear | 0.03 | [0, 1] |
| `suppression.confidence_threshold` | float | — | 0.6 | [0, 1] |
| `suppression.spectral.fft_size` | size | samples | 128 | 128 or 256 |
| `suppression.spectral.hop_size` | size | samples | 32 | 32 with 128, 64 with 256 |
| `suppression.spectral.gain_floor_db` | float | dB | −12 | [−80, 0] |

Resolve: if `enabled` is false, backend is **off** (no spectral delay). Old YAML
without `backend` remains conservative when enabled. Unknown backend strings
fail validation (not remapped).

CLI: `--suppression-backend off|conservative|spectral` on wav_replay and
stream_process. The testbench GUI exposes a capability-gated backend selector
and passes it on recorded, preview, and real-time paths. Conservative live
knobs (ambient floor, fade, activity, envelope) are disabled for spectral;
focus and confidence remain active. Stream protocol v3 live conservative knobs
are ignored when the spectral backend is selected, except confidence threshold.

Provenance (`sonitude_resolved`): backend requested/resolved, FFT, hop, gain
floor dB, algorithmic delay samples from the prepared processor,
`implementation_status: EXPERIMENTAL` only when the resolved backend is spectral.
`suppression_resolved` is true only when a processing backend is actually selected
(not `off`).

## Init / reset / invalid input / discontinuities

- `prepare` validates and allocates. `process` does not allocate.
- Reset restores STFT FIFOs, noise=`1` (uninitialized), gains=`1`, bypass mix=`0`.
- Non-finite input samples are replaced with 0 before the STFT.
- `sonitude_realtime`: after a capture XRUN skip, the next processed block
  holds noise-estimator updates (complete freeze).
- Stream gaps abort; they do not conceal. No firmware invalid-frame flag exists
  beyond ALSA XRUN skip.

## Evidence table

| Quantity | Label | Notes |
| --- | --- | --- |
| STFT reconstruction error | MEASURED (unit test) | `< 2e-4` abs after skip/tail |
| Algorithmic delay 128/32 | MEASURED | 127 samples first-arrival |
| Host block CPU | MEASURED | `sonitude_spectral_bench`; host only |
| Init/persistent RAM | MEASURED | `persistentBytes()` on host |
| End-to-end latency | NOT MEASURED | No loopback/impulse rig in this PR |
| Pico 2 W / RP2350 | NOT MEASURED | No target build or cycle counts |
| STM32H7 | NOT MEASURED | No CMSIS-DSP port, no board |
| Array geometry / 6ch health | INCOMPLETE | Do not claim beamforming improvement |

MCU mapping (unfilled): compiler, core, RAM/flash sections, FFT backend,
worst-case cycles, DMA, radio/USB (Pico); clock/cache/FPU, CMSIS-DSP version,
stack (H7) — all **NOT MEASURED**.

Host deadline utilization is a desktop measurement. It is **not** a project
pass/fail MCU gate.

## Limitations

- Mono Wiener postfilter cannot separate co-located sources with the same
  spectrum.
- Can colour speech and produce musical noise.
- First allowed update initializes `λ` from the median spectrum; a leading
  full-band utterance can still colour the estimate if it is not tonal versus
  the median.
- Not spatial, not distance estimation, not beamformer improvement.
- Testbench residual and intelligibility metrics delay-align the beamformed tap
  by `suppression_algorithmic_delay_samples` before subtraction.

## Disabled follow-up (not in this PR)

Target / near / far / off-axis **guard beams** and spatial spectral contrast
would compare post-beam spectra. Requires verified 6-channel health, calibration,
and geometry first. Do not implement here.

## Test / bench commands

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONITUDE_WITH_ALSA=OFF
cmake --build build --target sonitude_unit_tests sonitude_wav_replay sonitude_spectral_bench
./build/sonitude_unit_tests
./build/sonitude_spectral_bench
cd testbench && SONITUDE_BUILD_DIR=../build SONITUDE_REQUIRE_CPP=1 python -m pytest -q --tb=short
```
