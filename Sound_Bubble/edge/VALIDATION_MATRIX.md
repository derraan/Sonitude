# Sound Bubble Validation Matrix (Audit Pass)

This matrix replaces the lightweight checklist with testable pass/fail criteria for host runtime, firmware telemetry, and hardware timing validation.

## Host Runtime Validation

| Area | How to test | Pass criteria |
|---|---|---|
| ONNX export correctness | `python edge/export_to_onnx.py ... --output edge/model.onnx --contract-out edge/model.runtime.json` | ONNX export succeeds, checker passes, Torch-vs-ORT equivalence passes for all checked frames |
| Benchmark integrity | `python edge/benchmark.py --model edge/model.onnx --contract-path edge/model.runtime.json --runs 100` | Runs without shape errors; reports stable mean/std/p95 |
| Live inference continuity | `python edge/inference_pipeline.py --model edge/model.onnx --contract-path edge/model.runtime.json --profile --monitor ...` | No crashes, no continuous underruns, no sustained queue runaway |
| Overrun fallback | Stress CPU while live pipeline is running | Overrun log appears, audio continues via previous output chunk fallback |
| Input level validator | Inject silence, speech, loud speech, and full-scale fault-like input | Silence gating works over rolling window; full-scale faults are muted immediately |
| Drift correction | Run 10+ minutes with normal capture/playback | Drift ppm remains bounded and queue fill converges around target |

## Firmware Telemetry Validation (Pico)

Firmware repo: `c:\\Users\\darre\\mic-array-pico2w-usb6ch`

| Area | How to test | Pass criteria |
|---|---|---|
| DMA overrun visibility | Stage-6 CDC telemetry line in `usb_microphone_array_6ch` includes `ovr` and `ovr_bytes` | Counters are present and non-silent; nominal run keeps `ovr` at 0 |
| Clock divider visibility | Stage-6 CDC line includes `div=INT.FRAC` and `rate` | Divider/rate values are visible during bring-up |
| Clock-output drive strength | Verify firmware sets SCK/WS drive strength to 12mA | `gpio_set_drive_strength(..., GPIO_DRIVE_STRENGTH_12MA)` present and active on SCK/WS pins |
| Sample-rate support set | Build with 16k, 24k, 44.1k, 48k override values | Firmware builds and enumerates at configured rate |
| Buffer contract clarity | Inspect DMA/ring constants and comments | Buffer size and alignment intent are explicit and consistent with capture contract |

## Hardware Scope / Logic Analyzer Validation

| Target rate | Expected WS | Expected SCK | Divider target @192MHz |
|---|---:|---:|---|
| 16,000 Hz | 16.000 kHz | 1.024 MHz | INT=93, FRAC=192 |
| 24,000 Hz | 24.000 kHz | 1.536 MHz | INT=62, FRAC=128 |
| 44,100 Hz | 44.100 kHz | 2.8224 MHz nominal | INT=34, FRAC=3 (~+55 ppm) |
| 48,000 Hz | 48.000 kHz | 3.072 MHz | INT=31, FRAC=64 |

## End-to-End Acceptance Targets

- Host e2e latency under `30 ms` in steady-state run
- ONNX inference target in benchmark under project budget
- No silent DMA loss (overrun counters visible and monitored)
- Stable multichannel capture contract (active ch0..5, expected padding channels if applicable)
