# Sound Bubble ONNX Pipeline (Raspberry Pi)

This folder contains scripts for exporting the streaming Sound Bubble model to ONNX, benchmarking ONNX Runtime on CPU, and running the live 6-channel inference pipeline from Pico USB audio input.

**Field bring-up / update after merges:** see [`rpi5/PiSetup_BringUp.md`](rpi5/PiSetup_BringUp.md)
(Sound Bubble bootstrap, Sonitude C++ rebuild on the Pi, ALSA device selection, RT threading).

## 1) Install dependencies

```bash
python -m pip install --upgrade pip
python -m pip install -r edge/requirements_edge.txt
```

## 2) Export PyTorch model to ONNX

Using an explicit config + checkpoint:

```bash
python edge/export_to_onnx.py \
  --config-path real_experiments/raspberrypi_local_pretrain.json \
  --checkpoint-path /path/to/checkpoints/best.pt \
  --output edge/model.onnx \
  --contract-out edge/model.runtime.json
```

Using a run directory:

```bash
python edge/export_to_onnx.py \
  --run-dir /path/to/run_dir \
  --output edge/model.onnx \
  --contract-out edge/model.runtime.json
```

The script performs:
- ONNX export (`opset=17`, `do_constant_folding=True`)
- `onnx.checker.check_model(...)`
- numerical equivalence check between PyTorch and ONNX Runtime with `np.allclose(...)`
- runtime contract export (`*.runtime.json`) for benchmark/live scripts

## 3) Benchmark ONNX Runtime latency

```bash
python edge/benchmark.py \
  --model edge/model.onnx \
  --contract-path edge/model.runtime.json \
  --num-threads 4 \
  --warmup 20 \
  --runs 100 \
  --batch-size 1 \
  --channels 6 \
  --frame-len 288
```

This prints mean/std/p95 latency in milliseconds using `CPUExecutionProvider` and `ORT_ENABLE_ALL`.

## 4) Run live inference pipeline

Runtime architecture (threads, capture queue policy, CPU budget, overload fill, resets) is documented in **[`EDGE_RUNTIME.md`](EDGE_RUNTIME.md)**. Every CLI flag is listed in **[`INFERENCE_PIPELINE_ARGS.md`](INFERENCE_PIPELINE_ARGS.md)**.

List devices first:

```bash
python edge/inference_pipeline.py --model edge/model.onnx --list-devices
```

Run live pipeline:

```bash
python edge/inference_pipeline.py \
  --model edge/model.onnx \
  --contract-path edge/model.runtime.json \
  --input-device <pico_input_device_index> \
  --output-device <speaker_output_device_index> \
  --input-channels 8 \
  --output-channels 2 \
  --channel-map 0,1,2,3,4,5 \
  --input-sr 44100 \
  --model-sr 24000 \
  --output-sr 48000 \
  --model-chunk 192 \
  --model-pad 96 \
  --profile \
  --monitor
```

## Key runtime features

- Streaming ONNX state propagation (state inputs/outputs threaded every frame)
- Per-channel DC removal IIR:
  - `y[n] = x[n] - x[n-1] + 0.9999 * y[n-1]`
- Input level validation before inference:
  - `< -50 dBFS`: treat as silence/disconnected, zero-fill channel
  - `-50 .. -1 dBFS`: valid range, pass through
  - `-1 .. 0 dBFS`: digital fault, zero-fill channel and log event
- Preprocessing policy:
  - deterministic path for deployment: DC removal + level validation + optional fixed input gain
  - no adaptive AGC in `inference_pipeline.py` by default (to keep model input distribution stable during audit tests)
- Bounded realtime path: shallow capture queue (`--max-capture-blocks`), model ring (`--model-ring-ms`), output ahead cap/trim, optional **queue policy** and **overload fill** (see `EDGE_RUNTIME.md`).
- Slow-step visibility: steps slower than `--max-overrun-ms` increment an overrun counter in `perf` (see `INFERENCE_PIPELINE_ARGS.md`).

## Runtime contract

`export_to_onnx.py` writes a small JSON contract consumed by benchmark and live inference.
It keeps model-critical values in one place:

- `model_sr`
- `model_num_ch`
- `model_chunk`
- `model_pad`
- `frame_len`
- `expected_input_channels`
- `expected_channel_map`

This avoids drift between export assumptions and runtime CLI defaults.

## Notes

- Pico UAC2 firmware in this workspace currently streams 8 USB channels at 44.1 kHz, where channels 0-5 contain mic data and 6-7 are zero slots.
- For lower latency, reduce `--hops-per-io` and tune `--intra-op-threads`.

## Remote controller over Tailscale

The Raspberry Pi can run a small HTTP controller that manages `edge/inference_pipeline.py` over a private Tailscale network.

Start locally for testing:

```bash
export SOUND_BUBBLE_CONTROLLER_TOKEN='replace-with-long-random-token'
python -m edge.controller.app --host 127.0.0.1 --port 8000
```

Health check:

```bash
curl http://127.0.0.1:8000/health
```

Authenticated status:

```bash
curl -H "Authorization: Bearer $SOUND_BUBBLE_CONTROLLER_TOKEN" \
  http://127.0.0.1:8000/status
```

On the Pi, run it through `systemd` using `edge/rpi5/soundbubble-controller.service`.
Bind to `0.0.0.0` only when the API is protected by Tailscale/firewall rules and bearer-token auth.
