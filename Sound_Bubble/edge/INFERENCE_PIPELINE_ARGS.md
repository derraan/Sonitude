# `edge/inference_pipeline.py` — CLI Argument Reference

This document describes every CLI argument supported by `edge/inference_pipeline.py`, what it does, and when to change it.

For how those flags fit into threads, buffers, queue policy, overload fill, and timeline resets, see **[`EDGE_RUNTIME.md`](EDGE_RUNTIME.md)**.

## Model selection

### `--model <path>`

- **What**: Path to a single ONNX model file.
- **When**: Use for direct model runs (most common).
- **Notes**: Overrides `--zoo-dir` selection if both are provided.

### `--contract-path <path>`

- **What**: Path to the runtime contract JSON paired with the ONNX export.
- **When**: Use whenever you have it; it sets `model_sr`, `model_num_ch`, `model_chunk`, and `model_pad` from the export.

### `--zoo-dir <dir>`

- **What**: Directory containing paired `<name>.onnx` + `<name>.runtime.json`.
- **When**: Use when you want model selection by `--bubble-radius` without editing paths.

### `--bubble-radius <float>`

- **What**: Bubble radius in metres (typically `1.0`, `1.5`, `2.0`).
- **When**:
  - **Required** when using `--zoo-dir`.
  - **Recommended** when using a distance-aware ONNX that has a `dis_embed` input.

## Devices and channels

### `--input-device <int>`

- **What**: `sounddevice` device index for capture.
- **When**: Set to the real `hw:*` device index (not `default`, `dmix`, etc.).

### `--output-device <int>`

- **What**: `sounddevice` device index for playback.

### `--input-channels <int>`

- **What**: Number of channels requested from the input device.
- **Default**: `8`.

### `--output-channels <int>`

- **What**: Number of playback channels.
- **Default**: `2`.

### `--channel-map <csv>`

- **What**: Comma-separated channel indices to select from the input stream before feeding the model.
- **Default**: `"0,1,2,3,4,5"`.

### `--monitor-input-channel <int>`

- **What**: Which input channel is treated as the reference “dry” channel for `--mix-dry`.
- **Default**: `0`.

### `--list-devices`

- **What**: Lists audio devices and exits.
- **When**: Always run once on a new Pi image to confirm indices and default rates.

## Rates, chunking, and PortAudio stream config

### `--input-sr <int>`

- **What**: Input sample rate (Hz).
- **Default**: `44100`.

### `--model-sr <int>`

- **What**: Model sample rate (Hz).
- **Default**: `24000`.
- **Notes**: The model path runs at this **fixed** rate.

### `--output-sr <int>`

- **What**: Output sample rate (Hz).
- **Default**: `48000`.
- **Notes**: Must be supported by the playback device (`/proc/asound/cardN/stream0` is source of truth on Linux).

### `--model-chunk <int>`

- **What**: Hop size (samples at `--model-sr`) processed per recurrent step.
- **Default**: `192`.

### `--model-pad <int>`

- **What**: Extra pad samples included in the model’s rolling window.
- **Default**: `96`.

### `--model-num-ch <int>`

- **What**: Number of channels expected by the model tensor.
- **Default**: `6`.

### `--hops-per-io <int>`

- **What**: How many model hops are grouped per audio I/O block.
- **Default**: `2`.
- **Effect**: Larger values increase buffering and jitter tolerance but increase latency.

### `--latency {low,high}`

- **What**: `sounddevice` latency hint.
- **Default**: `low`.

## Realtime contract (bounded buffering)

These prevent **seconds-long accumulation** by bounding buffers and dropping/skip-forward under load.

### `--max-capture-blocks <int>`

- **What**: Capacity of the capture buffer in I/O blocks.
- **Default**: `2`.
- **Behavior**: Drop-oldest on overflow (keeps newest audio).

### `--max-output-ahead-sec <float>`

- **What**: Maximum allowed unread audio queued in the output ring (seconds).
- **Default**: `0.10`.
- **Behavior**: If exceeded, playback trims oldest queued audio to recover.

### `--target-output-ahead-sec <float>`

- **What**: Target output ahead after a skip (seconds).
- **Default**: `0.04`.

### `--rt-fifo-priority <int>`

- **What**: Best-effort attempt to set process scheduling to `SCHED_FIFO` at this priority.
- **Default**: `0` (disabled).
- **When**:
  - Leave `0` if you already run via `sudo chrt -f <prio> ...`.
  - Set to `50` (or similar) only if you have permissions (sudo or `cap_sys_nice`).

## Signal conditioning and protection

### `--output-gain <float>`

- **What**: Linear gain applied to output.
- **Default**: `1.0`.

### `--mix-dry <float>`

- **What**: Mix a resampled “dry” reference channel into the output.
- **Default**: `0.0`.
- **Range**: `0.0` (wet only) → `1.0` (dry only).

### `--input-gain-db <float>`

- **What**: Gain applied to model input in dB.
- **Default**: `0.0`.

### `--alpha <float>`

- **What**: DC offset IIR coefficient.
- **Default**: `0.9999`.

### `--silence-dbfs <float>`

- **What**: Rolling RMS threshold for “silence” detection; channels below are substituted with zeros.
- **Default**: `-50.0`.

### `--silence-window-frames <int>`

- **What**: Rolling window size (frames) used to decide silence/fault.
- **Default**: `8`.

### `--fault-min-dbfs <float>` / `--fault-max-dbfs <float>`

- **What**: Range that marks a “FAULT” channel; channels in-range are substituted with zeros.
- **Defaults**: `-1.0` to `0.0`.

## ONNX Runtime and timing safety

### `--max-overrun-ms <float>`

- **What**: If a single ONNX step exceeds this wall time (ms), the pipeline counts an **overrun** in `perf` (visibility only for this script path).
- **Default**: `25.0`.
- **Notes**: The live worker still uses the step’s actual output tensor; tune this flag for logging and diagnosis, not as a hard substitute policy.

### `--intra-op-threads <int>`

- **What**: ONNX Runtime intra-op threads.
- **Default**: `2`.
- **Pi guidance**: Use **cores−1** (e.g. `3` on Pi 5) so audio callbacks have CPU time.

### `--inter-op-threads <int>`

- **What**: ONNX Runtime inter-op threads.
- **Default**: `1`.

## Logging / visibility

### `--profile`

- **What**: Prints a periodic summary line with timing, buffer health, queue policy signals, and inference totals.
- **Fields (high level)**: `mode` (or `starved` when `steps==0` and output ahead is near zero), `capture_age` / `capture_gap` / `capture_drained`, stage timings (`capture->frame`, `dc`, `level-valid`, `frame->infer`, `post+out`, `e2e`), `infer_mean` / `infer_total` / `steps`, `q` / `max_capture_blocks`, `drops_oldest` on capture, `out_ahead`, output drops, `underrun_frames`, `model_ring` fill, model ring drops/resets.
- **Details**: See the “Profiling” section in [`EDGE_RUNTIME.md`](EDGE_RUNTIME.md).

### `--profile-interval <float>`

- **What**: Seconds between profile prints.
- **Default**: `1.0`.

### `--monitor`

- **What**: Prints per-channel dBFS/status periodically.
- **Warning**: Over SSH, lots of stdout can worsen timing.

### `--monitor-interval <float>`

- **What**: Seconds between monitor prints.
- **Default**: `0.5`.

## Backlog guardrails (model ring)

These bound the *model-side* buffering so the worker cannot integrate seconds of delay.

### `--model-ring-ms <float>`

- **What**: Capacity of the model input ring in milliseconds at `--model-sr`.
- **Default**: `64.0`.
- **Behavior**: Drop-oldest on overflow.

### `--max-steps-per-block <int>`

- **What**: Maximum ONNX steps executed per capture block iteration.
- **Default**: `4`.
- **Behavior**: Caps “catch-up” work to prevent runaway CPU spirals.

### Reset-on-drop behavior

- **What**: When the model ring drops audio (overflow), the pipeline resets recurrent ONNX state to zero **unconditionally**.
- **Why**: Overflow implies a discontinuity; keeping advanced recurrent state across dropped samples is invalid for streaming.

## Capture queue and adaptivity

These control **which** capture block the worker processes next and when a **timeline shed** is declared.

### `--queue-policy {fifo,latest,adaptive}`

- **What**: Service discipline for the bounded capture buffer.
- **Default**: `adaptive`.
- **Behavior**:
  - `fifo`: strict `pop` per wait.
  - `latest`: `pop_latest` (newest block wins; older blocks drained in one wait).
  - `adaptive`: same dequeue as `latest`, plus stale-age shedding (see `--max-capture-age-ms`).

### `--max-capture-age-ms <float>`

- **What**: In `adaptive` mode, if the dequeued block’s capture age exceeds this many ms, treat as discontinuity (shed path).
- **Default**: `24.0`.

### `--recover-steps-per-block <int>`

- **What**: Extra hop budget requested while in **recover** (output ahead low, capture still fresh, and sustainable budget allows it).
- **Default**: `2`.
- **Notes**: Capped by `--hops-per-io` and `--max-steps-per-block`. The worker also caps wet hops using an internal **block CPU budget** (EMA of per-stage costs); recover is skipped if the budget cannot afford `recover_target` hops.

### `--recover-ahead-multiplier <float>`

- **What**: While in **recover**, relaxes the output trim target (multiplier on target ahead in ms) so the ring can hold a bit more audio while catching up.
- **Default**: `1.5`.

## Overload / graceful degradation

When fewer than `hops_per_io` wet hops complete in an iteration (budget, ring underrun, etc.), the worker can append output-length **fill** so the output clock keeps moving.

### `--overload-policy {dry_fill,silence_fill,strict}`

- **What**: How to fill missing wet time for the current block.
- **Default**: `dry_fill`.
- **Behavior**:
  - `dry_fill`: append dry monitor-channel audio at output rate (from `DryReferenceRing.read_into_output`); zeros pad if the dry ring underruns.
  - `silence_fill`: append zeros for the same duration.
  - `strict`: no fill; shorter output blocks (more underrun risk under overload).

After dry/silence fill, the worker schedules a **deferred** timeline reset (model + resamplers + model ring) **without** clearing audio already queued on the output ring, so playback is not wiped immediately after enqueue. See “Timeline resets” in [`EDGE_RUNTIME.md`](EDGE_RUNTIME.md).

## Related documentation

- **[`EDGE_RUNTIME.md`](EDGE_RUNTIME.md)** — end-to-end runtime architecture (threads, buffers, policies, resets).
- **[`README_EDGE.md`](README_EDGE.md)** — export, benchmark, and legacy live example commands.
