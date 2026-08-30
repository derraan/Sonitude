# Experimental RP2350 MVDR port

This target is an evidence-gated embedded port of PR #34's narrowband MVDR fast path. It is not part of the default firmware build and is not a production-latency or speech-separation claim.

## Build

Use Pico SDK 2.1 or newer and an unmodified checkout of ARM's CMSIS-DSP repository:

```sh
cmake -S mic-array-pico2w-usb6ch -B build-pico-mvdr \
  -DSONITUDE_BUILD_EXPERIMENTAL_MVDR=ON \
  -DCMSISDSP_ROOT=/absolute/path/to/CMSIS-DSP
cmake --build build-pico-mvdr --target experimental_mvdr_pico2w
```

`-O3` is enabled only for the experimental target. `-ffast-math` is opt-in with `-DSONITUDE_DSP_FAST_MATH=ON`; it must remain off for numerical comparison and covariance-conditioning tests.

## What is implemented

- six-channel 128-point CMSIS real FFT/iFFT with a periodic Hann window;
- 32-sample hop (75% overlap), matching the host PR #34 backend rather than the prompt's incompatible 50% overlap;
- fixed-size, allocation-free fast-path storage after initialization;
- CMSIS complex multiply for applying MVDR weights;
- complex 6x6 covariance represented correctly as a real 12x12 system before `arm_mat_inverse_f32`;
- relative diagonal loading and covariance adaptation, both disabled until measured parameters are supplied;
- shared double-buffer mailboxes with FIFO generation tokens instead of sending matrices through the small hardware FIFO;
- smoothed fast-OVD gain logic, disabled until the correct front/rear channel pair, ratio distribution, and policy gain are measured;
- the existing PIO/I2S driver's DMA ping-pong capture path. DSP runs outside its interrupt handler.

## Evidence gates and known gaps

| Item | Status | Required evidence |
|---|---|---|
| INMP441 input | Implemented | The input is 24-bit I2S carried in 32-bit integer words, then converted to float. It is not PDM or float DMA. |
| STFT topology | Implemented | Preserve 128/32 until a 128/64 perfect-reconstruction and impulse-delay test exists. |
| MVDR math | Implemented, disabled adaptation | Calibrated complex steering vectors; covariance time constant; singular-bin and distortion tests. |
| Own-voice fast gate | Scaffolded, disabled | Measured front/rear channel mapping and own-voice/non-own-voice ROC; click-free attack/release listening test. |
| Mouth verification beam | Not enabled | A measured near-field mouth transfer function and decision threshold are absent. Only the release token contract exists. |
| Guard looks/postfilter | Not ported | PR #34's three guard spectra and target-versus-guard spectral gain should be fused into this STFT after the base port passes. |
| SRAM4/SRAM5 pinning | Not claimed | The Pico SDK 2.1 default linker script does not provide those application sections. Add a custom linker layout only after map-file and bus-contention measurements. |
| ANC codec output | Hook only | Implement the non-blocking `sonitude_anc_write_block()` board adapter and measure its buffering. |
| Latency | Unverified | The host 128/32 STFT first-arrival is 127 samples. At 48 kHz that is 2.65 ms for one STFT, but capture, 64-frame scheduling, compute, codec, and any stacked postfilter still require an impulse measurement. The prompt's 6.05 ms is not treated as measured. |
| CPU/RAM | Unverified | Record worst-case cycles per 32-sample hop, stack high-water mark, map-file SRAM use, DMA overruns, and thermal/clock configuration on a Pico 2 W. |

The current driver copies completed DMA halves into a ring in interrupt context. A future zero-copy adapter may reduce memory traffic, but it must retain a short ISR and explicit overrun telemetry. Do not run FFTs or matrix inversion inside the DMA handler.
