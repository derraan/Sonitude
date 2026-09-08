# Sound Bubble edge runtime (`inference_pipeline.py`)

This document describes how the live ONNX pipeline is structured on the wire: threads, buffers, queue policy, overload degradation, and timeline resets. CLI flags are summarized here; the full flag list is in [`INFERENCE_PIPELINE_ARGS.md`](INFERENCE_PIPELINE_ARGS.md).

## Goals

End-to-end path: **capture → validate/process channels → resample to model rate → ONNX streaming inference → resample/output → live playback**.

- **Bounded latency**: shallow capture buffer, bounded model ring, bounded output ahead with explicit trim.
- **Fresh capture under load**: adaptive service discipline avoids processing arbitrarily stale capture blocks (no FIFO-dominated stale backlog).
- **Coherent state after gaps**: discontinuities reset streaming model state and resamplers; output already queued for playback is preserved when resetting after a delivered block.
- **Graceful degradation**: when the worker cannot afford full wet hops for the current I/O block, optional **dry** or **silence** fill keeps the output clock fed while model state is reset for future wet steps.
- **No unbounded catch-up**: the worker does not try to “process the whole queue” inside one block; policy is deadline-aware and latest-biased under overload.
- **Lightweight callbacks**: input/output PortAudio callbacks only move samples and set flags—no inference, resampling, resets, or logging on the callback thread.

### Policy summary

The runtime is **queue-time adaptive**, **deadline-aware**, and **graceful-degradation capable** (not only static knob tuning). Priority order:

1. Do not let stale capture backlog dominate latency; prefer **latest** audio over strict FIFO when policy demands it.
2. Run **wet** inference only within the measured step budget; **recover** extra hops only when the sustainable budget allows.
3. If full wet work does not fit the block, **fill** the missing timeline (dry or silence) and **reset** recurrent state so future wet steps stay coherent.
4. Keep **continuous output** even when effect quality is partial under overload.

### Pipeline block diagram

```mermaid
flowchart LR
  %% ===================== THREADS =====================
  subgraph IN["Audio input callback thread (PortAudio)"]
    A1[Capture audio block\n(input_sr, input_channels)] --> A2[Timestamp + capture_idx]
    A2 --> A3[DropOldestBuffer\n(max_capture_blocks)\noverflow: drop oldest]
  end

  subgraph WK["Worker thread (owns DSP + ONNX)"]
    B1{Dequeue\n--queue-policy\nfifo | latest | adaptive}
    B1 -->|pop / pop_latest| B2[Capture item\n{x, t_capture, capture_idx}]

    B2 --> B3[Validate / DC / level gates\n(non-infer stage timing)]
    B3 --> B4[Resample input → model_sr\n(in_src)]
    B4 --> B5[Assemble model timeline frames\nmodel_chunk × hops_per_io]

    %% -------- budget + policy state machine --------
    B5 --> P1[Budget estimator\nblock_budget_ms\ninfer_ema_ms + noninfer_ema_ms\nreserve_ms]
    P1 --> P2[Step budget cap\nsteps_run ≤ min(hops_per_io,\nmax_steps_per_block,\nsustainable_steps)]
    P2 --> P3{Mode\nnormal | recover | shed}

    P3 -->|shed triggers:\ndrained_blocks>0\ncapture_gap>0\n(adaptive) age>max_age| R0[Defer reset_stream_timeline\n(reason=capture-discontinuity)]
    P3 -->|recover when affordable:\nneed_recover AND\nsustainable_steps ≥ recover_target| P4[Recover stepping\n(looser ahead target)]
    P3 -->|normal| P5[Steady stepping]

    %% -------- model ring + streaming ONNX --------
    P4 --> MR
    P5 --> MR
    MR[Model ring buffer\n(model_ring_ms bound)] --> O1[ONNX streamer\nstreaming inference]
    O1 --> O2[Wet output @ model_sr]

    %% -------- overload fill + deferred reset --------
    O2 --> S1{Missing wet hops?\nmissing_steps = hops_per_io - steps_run}
    S1 -->|no| POST
    S1 -->|yes| F1[Overload fill tail\n--overload-policy\n dry_fill | silence_fill | strict]
    F1 --> R1[Defer reset_stream_timeline\n(reason=missing-wet-hops-filled)]
    F1 --> POST

    %% -------- post + output ring + trim --------
    POST[Post-process\nresample model_sr→output_sr (out_src)\noptional dry mix (--mix-dry)\ngain/clip\nexpand to output_channels] --> OUTW
    OUTW[OutputRingBuffer\nwrite_drop_oldest] --> TRIM[Ahead trim to max_output_ahead\n(normal vs recover target)]
    TRIM --> DRYLOCK[DryReferenceRing\n(drop_oldest_frames lockstep)]

    %% deferred reset (does NOT clear already-queued output)
    DRYLOCK -->|if pending| RESET[reset_stream_timeline\n(clear_io_rings=false)]

    %% hard reset path (clears IO rings)
    MR -->|overflow/drop| RESET_HARD[reset_stream_timeline\n(clear_io_rings=true)\n(clear output_ring + dry_ref)]
  end

  subgraph OUT["Audio output callback thread (PortAudio)"]
    C1[Output callback] --> C2[OutputRingBuffer.read]
    C2 --> C3[Play to device\n(output_sr, output_channels)]
  end

  subgraph MAIN["Main thread"]
    M1[Start streams + worker] --> M2[Monitor sticky flags\n(input/output/worker exceptions)]
    M2 --> M3[Shutdown + cleanup]
  end

  %% ===================== CROSS-THREAD LINKS =====================
  A3 --> B1
  OUTW --> C2
```

---

## Realtime budget (why adaptive policy exists)

With `model_chunk = 192` and `model_sr = 24000`, one model hop is **192 / 24000 ≈ 8 ms** wall time at the model rate—about **125 hops/s** if you could run them back-to-back.

With `hops_per_io = 2`, one PortAudio input block spans **2 × 8 ms = 16 ms** of audio at the model timeline, and the worker targets up to **two** wet hops per block.

If measured wet cost is roughly **`infer_mean ≈ 8.5 ms` per hop**, then **two hops ≈ 17 ms** of inference time on a **16 ms** block budget. Full “always run `hops_per_io` wet hops” is then **not** sustainably real-time without headroom from a faster model, larger hop, or lower load. Logs on this stack have shown recover paths around **~17.0–17.3 ms** `infer_total` against a **16 ms** block—exactly this margin problem.

**Implication:** queue policy alone cannot create compute headroom. The combination of **step budget capping**, **recover only when affordable**, **overload fill**, and **timeline reset** is what keeps live audio stable when wet inference is marginal.

---

## Failure modes this design avoids

### Stale FIFO backlog

Symptoms: capture queue stays nearly full, `capture->frame` latency grows into hundreds of ms, `drops_oldest` rises, underruns continue. The worker is still “working,” but on **stale** audio—unacceptable for live monitoring.

### Fresh but still unstable

Shallow capture (`max_capture_blocks` small) fixes capture age, but if **wet** work still exceeds the block budget, you can still see rising `drops_oldest`, `underrun_frames`, and `out_ahead` scraping zero. The pipeline needs **overload fill** and **budget**, not only a shorter queue.

### Recover oscillation (too many recover hops)

If `recover_steps_per_block` (and the sustainable budget) allow **three** hops on a **16 ms** block while each hop is ~8.5 ms, **recover** itself blows the deadline → capture gap → **shed** → **recover** loop. Keep recover hops aligned with **measured** sustainable steps (and cap with `max_steps_per_block` / `hops_per_io`).

---

## Threads and ownership

| Context | Role |
|--------|------|
| **Audio input callback** | Copies one capture block, timestamps it, pushes to `DropOldestBuffer`. Sets sticky flags on status/exception. No prints, no shutdown orchestration. |
| **Audio output callback** | Reads one block from `OutputRingBuffer` into PortAudio’s buffer. Sets sticky flags on failure. |
| **Worker thread** | Owns resampling, DC/level, model ring, ONNX, wet/dry mix, output ring writes, trim, queue/overload policy, and deferred timeline resets. Wrapped in `try/except` with sticky `worker_exception` to the main thread. |
| **Main thread** | Opens streams, sleeps in a loop, reacts to sticky flags (stop on worker/input/output exceptions), handles signals and cleanup. |

### Audio callback contract

Treat PortAudio callbacks like a **minimal top-half** (ISR-style):

**Allowed:** copy one input block; push to bounded queue; read output ring into `outdata`; zero-fill on exception; set sticky flags; return.

**Forbidden:** `print()`, traceback formatting, shutdown orchestration, heavy allocation, model inference, resampling, streaming state reset, or any work that should live in the worker.

---

## Block cadence

Per iteration the worker consumes **one** capture item (one PortAudio input block). Block sizes in samples are derived from model hop geometry:

- `model_io_chunk = model_chunk * hops_per_io` (at model rate)
- `input_block = round(model_io_chunk * input_sr / model_sr)`
- `output_block = round(model_io_chunk * output_sr / model_sr)`

Example (48 kHz in, 24 kHz model, `model_chunk=192`, `hops_per_io=2`): **16 ms** of wall clock per input block at the input device.

The worker targets up to **`hops_per_io`** ONNX hops per block (`step_budget` may be lower under budget policy).

---

## Capture path

1. **Buffer**: `DropOldestBuffer` with capacity `max_capture_blocks` (from `RealtimeContract`). Overflow **drops oldest**, keeps newest.
2. **Item shape**: each item is a small dict: `x` (float32 capture), `t_capture` (`perf_counter()`), `capture_idx` (monotonic integer).
3. **Dequeue**:
   - **`fifo`**: `pop(timeout)` — strict FIFO.
   - **`latest` / `adaptive`**: `pop_latest(timeout)` — wait for first item, then drain without blocking to the **newest** item; returns `(item, drained_count)`.

## Queue policy (`--queue-policy`)

| Mode | Behavior |
|------|-----------|
| `fifo` | One block per wait; no intra-wait draining. |
| `latest` | Always `pop_latest`: prefer freshest block; `drained_blocks` counts skipped older blocks. |
| `adaptive` (default) | Same dequeue as `latest`; additionally treats **stale** capture (capture age above `--max-capture-age-ms`) as a discontinuity (see below). |

**Recommended for live demos:** `--queue-policy adaptive`.

**Rule of thumb:** queue **length** alone is a weak signal; combine it with **capture age**, **gap**, **drain**, and **inference EMAs** when tuning.

### Modes tracked in `queue_policy["mode"]`

- **`normal`**: steady step budget from measured CPU budget.
- **`recover`**: when adaptive, output ahead is below target, capture is still “fresh”, and sustainable budget allows at least `recover_target` hops — uses `--recover-steps-per-block` (capped by `hops_per_io` and `max_steps_per_block`) and a looser output trim target via `--recover-ahead-multiplier`.
- **`shed`**: capture discontinuity (gap, drain, or adaptive stale age) — full timeline reset semantics for the **streaming** path; see resets below.

### Capture discontinuity

A discontinuity is flagged when any of:

- `drained_blocks > 0` (latest-wins skipped audio),
- `capture_gap > 0` (non-contiguous `capture_idx`),
- `adaptive` and `capture_age_ms > max_capture_age_ms`.

On discontinuity the worker enters **`shed`** and schedules a **deferred** `reset_stream_timeline` after the current block’s output is written (see resets).

### Policy signals (profiling / tuning)

Useful fields when reading `--profile` lines or logs:

`capture_age_ms`, `capture_gap`, `capture_drained`, `out_ahead_ms`, `infer_mean_ms`, `infer_total_ms`, `steps` (hops run), `model_ring` fill/drops, `underrun_frames`, `drops_oldest`, queue `q` vs `max_capture_blocks`, and internal `mode` (`normal` / `recover` / `shed`, or `starved` when masked—see Profiling).

---

## CPU budget and step budget

The worker estimates how many wet hops fit in one input block:

- `block_budget_ms = 1000 * input_block / input_sr`
- `infer_ema_ms` and EMAs for DC, level, frame→infer, post (updated each iteration; infer EMA only when at least one hop ran).
- `safe_budget_ms = block_budget_ms - noninfer_ema_ms - reserve_ms` (1 ms reserve)
- `sustainable_steps = floor(safe_budget_ms / infer_ema_ms)` (clamped at ≥ 0)
- `steady_target_steps = min(hops_per_io, sustainable_steps, max_steps_per_block)`

Conceptually: `step_budget = min(hops_per_io, sustainable_steps)` for the steady path; **recover** may only ask for extra hops when `sustainable_steps` supports `recover_target`.

**Recover** runs only if `need_recover` (adaptive, not shed, fresh capture, low output ahead) **and** `sustainable_steps >= recover_target`, where `recover_target = min(recover_steps_per_block, hops_per_io, max_steps_per_block)`.

While **`shed`**, the worker does not flip back to normal inside the budget block; it uses `steady_target_steps` for that iteration so shed is visible across the policy state machine.

**Critical lesson:** if two wet hops routinely cost more than one block’s budget, **two-hop full wet** is not sustainable at that cadence; expect **missing hops**, **fill**, and **resets** unless inference or hop geometry improves.

---

## Overload policy (`--overload-policy`)

After the stepping loop, `missing_steps = max(0, hops_per_io - steps_run)` (hops that were **not** executed wet, e.g. ring underrun or budget capped at 0).

- **`dry_fill` (default)**: append `missing_steps * frames_per_hop_out` output frames read from the dry timeline (`DryReferenceRing.read_into_output`), replicated to output channels. Keeps **continuous** output when wet cannot fill the block; effect may be partially bypassed for those frames.
- **`silence_fill`**: append zeros for the same frame count.
- **`strict`**: no synthetic tail; output may be short (more underrun risk).

When fill is used, a **deferred** timeline reset is scheduled so recurrent state matches the fact that those hops were not processed wet.

`frames_per_hop_out = round(output_sr * (model_chunk / model_sr))` (e.g. 384 frames at 48 kHz for `model_chunk=192` at 24 kHz model).

### Missing wet hops and deferred reset

Do **not** clear output already written for this cycle before handling fill. The implementation sets `pending_timeline_reset_reason` to `"missing-wet-hops-filled"` when overload fill runs without a capture discontinuity on the same block. If both discontinuity and fill apply in one iteration, the deferred reason is **`capture-discontinuity`** (single reset covers both). After `write_drop_oldest` and ahead trim, `reset_stream_timeline(..., clear_io_rings=False)` runs so playback already queued to the output ring is not wiped.

---

## Output path and dry lockstep

- Wet audio is resampled to `output_sr`, optionally mixed with dry (`--mix-dry`), gain/clipped, then expanded to `output_channels` as `y_out` `[T, C]`.
- **`OutputRingBuffer.write_drop_oldest`**: if the ring is full, oldest frames are dropped before write; counter `out_ring_dropped_frames` increments.
- **Ahead trim**: if `ahead_frames > ring_max_ahead_frames`, trim to `effective_target_ahead_frames` derived from `effective_target_ahead_ms` (normal vs recover multiplier). **`dry_ref.drop_oldest_frames`** must match wet drops so dry and wet stay time-aligned. Do not rely on ring capacity alone—explicit max-ahead prevents latency ballooning.

---

## Timeline resets (`reset_stream_timeline`)

Worker-local reset (see `inference_pipeline.py`):

- `in_src.reset()`, `out_src.reset()`, `onnx_streamer.reset_state_and_warm()`
- `current_frame.fill(0.0)`, `last_output_chunk[0].fill(0.0)`
- `model_ring.clear()` when supported
- If `clear_io_rings=True` (model ring drop path): `output_ring.clear()`, `dry_ref.clear()`
- If `clear_io_rings=False` (capture discontinuity or missing-wet fill, after enqueue): streaming tensors and model ring still reset, but **output** and **dry** rings keep already-queued audio for the callback.

| Trigger | `clear_io_rings` | Effect |
|---------|------------------|--------|
| **Model ring overflow** (`model_ring` drop) | **true** | Reset input/output resamplers, ONNX state, frame tensors, clear **model ring**, **output ring**, and **dry** reference. |
| **Capture discontinuity** or **missing-wet fill** (deferred, after write/trim) | **false** | Same streaming resets and model ring clear, but **does not** clear `output_ring` or `dry_ref` so audio already queued for the callback is not wiped right after enqueue. |

Every reset increments `perf["capture_discontinuities"]` (metric name is historical; it counts coherent timeline resets, not only capture gaps).

---

## Dry reference ring

`DryReferenceRing` holds monitor-channel audio resampled to output rate for `--mix-dry` and for **`read_into_output`**: read up to *N* frames of mono dry into a user buffer `[N, C]` with channel replication.

---

## Profiling (`--profile`)

Periodic lines include:

- **`mode`**: internal policy mode, or **`starved`** when `steps_run == 0` and output ahead ≤ ~0.1 ms (readout hides “recover” masking starvation).
- **Capture**: `capture_age`, `capture_gap`, `capture_drained`, `capture->frame`.
- **Pipeline stages**: `dc`, `level-valid`, `frame->infer`, `post+out`, `e2e`, `drift`.
- **Inference**: `infer_mean`, `infer_total`, `steps`.
- **Queues**: `q` / `max_capture_blocks`, `drops_oldest` (capture buffer), `out_ahead`, output drops, `underrun_frames`, `model_ring` fill, model ring drops/resets.

### What “healthy” vs “unhealthy” often looks like

**Good signs:** `capture_age` well under the block period most of the time, shallow `q` (e.g. 0/1 or 1/1), `mode` mostly `normal`/`recover`, `shed` occasional, `out_ahead` not pinned at 0 ms, `underrun_frames` growth slow, audio feels continuous.

**Acceptable under overload:** partial wet effect, dry-filled segments, occasional shed/reset, minor discontinuities.

**Bad signs:** `capture_age` chronically high (e.g. 50+ ms), `q` stuck full, `recover` missing deadlines every block, `infer_total` routinely above `block_budget_ms`, `out_ahead` stuck at 0 ms, `underrun_frames` climbing fast.

---

## Recommended smoke-test command (Linux / Pi, from repo root)

Adjust device indices and paths as needed:

```bash
.venv/bin/python edge/inference_pipeline.py \
  --rt-fifo-priority 50 \
  --model edge/zoo/model.onnx \
  --contract-path edge/zoo/model.runtime.json \
  --input-device 0 \
  --output-device 1 \
  --input-channels 6 \
  --output-channels 2 \
  --channel-map 0,1,2,3,4,5 \
  --input-sr 48000 \
  --model-sr 24000 \
  --output-sr 48000 \
  --input-gain-db 20 \
  --silence-dbfs -100 \
  --latency low \
  --hops-per-io 2 \
  --max-steps-per-block 2 \
  --recover-steps-per-block 2 \
  --recover-ahead-multiplier 1.10 \
  --max-capture-blocks 1 \
  --queue-policy adaptive \
  --max-capture-age-ms 24 \
  --model-ring-ms 64 \
  --target-output-ahead-sec 0.040 \
  --max-output-ahead-sec 0.080 \
  --overload-policy dry_fill \
  --intra-op-threads 2 \
  --inter-op-threads 1 \
  --mix-dry 0 \
  --profile --profile-interval 1.0
```

On Windows, use the same flags with `python edge\inference_pipeline.py` (or your venv interpreter) and suitable device IDs.

---

## Practical limits and longer-term levers

If `infer_total` for `hops_per_io` wet hops routinely exceeds `block_budget_ms`, **no queue policy** can make **full** wet processing continuous at that cadence. The overload path trades **effect continuity** for **clock continuity** (dry/silence fill + state reset).

Longer-term ways to regain headroom: **larger effective hop** (e.g. more samples per ONNX call), **lighter or quantized model**, **faster runtime** (different EP, native hot path), or **hardware acceleration**. Until then, **adaptive queue + deadline-aware steps + dry fill** is the practical way to keep the demo usable.

---

## Guardrails (do not regress)

- No raw shared output ring across threads without the existing ring discipline.
- No logging, traceback formatting, or shutdown orchestration in audio callbacks.
- No FIFO-only policy as the default for live latency (prefer `adaptive` / `latest` for freshness).
- No continuation of recurrent **hidden state** across **missing** or **dropped** audio without a coherent **reset**.
- No stale-output replay that disagrees with advanced model state.
- No unbounded “catch up” work inside one callback or one worker spin.
- No huge output-ahead buffer masquerading as low latency—use **explicit max ahead** and trim, with **dry_ref** lockstep on drops.

---

## Related files

| File | Role |
|------|------|
| [`realtime/drop_oldest_buffer.py`](realtime/drop_oldest_buffer.py) | Bounded capture queue; `pop`, `pop_latest`, `qsize`. |
| [`realtime/ring_buffer.py`](realtime/ring_buffer.py) | Model `AudioRingBuffer`; playback `OutputRingBuffer` with `drop_oldest_frames`. |
| [`realtime/dry_reference.py`](realtime/dry_reference.py) | Dry timeline + `read_into_output`. |
| [`realtime/contracts.py`](realtime/contracts.py) | `RealtimeContract` validation for capture/output ahead bounds. |
| [`realtime/audio_io.py`](realtime/audio_io.py) | Stream construction and preflight. |
| [`pipeline/onnx_streamer.py`](pipeline/onnx_streamer.py) | ONNX session and `reset_state_and_warm`. |

---

## Summary

The edge runtime is intentionally moving from a **static** “always `hops_per_io` wet hops” picture to an **adaptive, deadline-aware live audio** pipeline: **if full wet fits the budget, run it; if not, run what fits, fill the rest, and reset future model timeline; never prioritize processing stale capture over freshness.** That ordering—**fresh input, bounded latency, continuous output, graceful quality loss under overload**—is what matters for real-time demos on marginal hardware.
