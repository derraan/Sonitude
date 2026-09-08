# Remote control plan (RPi + Tailscale)

This document proposes a simple, reliable way to **remotely control** the Sound Bubble edge runtime on a headless Raspberry Pi (e.g., Pi 5) from a laptop **without inbound SSH**, using **Tailscale**.

The core idea is:

- **Pi runs a small “controller” service** (HTTP API) that starts/stops/restarts `edge/inference_pipeline.py` with chosen CLI flags and streams back status + recent profiling lines.
- **Laptop runs a GUI** (PyQt or local web UI) that calls the controller API over the Pi’s **Tailscale IP / MagicDNS name**.

---

## Goals

- Control the runtime remotely on restrictive networks (e.g., phone hotspot) where inbound SSH is blocked.
- Make parameter sweeps safe and repeatable (e.g., latency tuning: `--hops-per-io`, `--max-capture-blocks`, output-ahead caps).
- Provide live observability (latest `--profile` metrics, “running / stopped”, current args).
- Keep security simple: **only devices in the tailnet can access the API** (optionally locked further via Tailscale ACLs).

Non-goals:

- Hot-applying low-level model parameters mid-stream. Most knobs are startup flags; we prefer **restart-with-new-args**.

---

## Architecture overview

### Components

- **Pi (headless)**
  - `soundbubble-controller` (new): HTTP service
  - `inference_pipeline.py`: the existing edge runtime process managed by controller
  - `tailscaled`: provides stable tailnet connectivity

- **Laptop**
  - Tailscale client
  - GUI app (PyQt) *or* browser-based UI hitting the controller API

### Network path

Laptop → (Tailscale WireGuard/DERP as needed) → Pi → `soundbubble-controller` → manages `edge/inference_pipeline.py`

Because the Pi initiates outbound connections for Tailscale, this works well on hotspots and CGNAT.

---

## Tailscale setup (Pi + laptop)

### Pi

1. Install Tailscale.
2. Bring it up and authenticate:

```bash
sudo tailscale up
```

3. Confirm the Pi has a tailnet IP:

```bash
tailscale ip -4
```

Optional (recommended):

- Enable **MagicDNS** in the Tailscale admin console so the Pi is reachable as a hostname (e.g., `rpi5-soundbubble`).
- Use **Tailscale ACLs** to restrict who can reach the API port.

### Laptop

1. Install Tailscale and sign in to the same tailnet.
2. Test reachability:
   - Ping Pi’s tailnet IP
   - Or open `http://<pi-tailscale-ip>:<port>/health` in a browser (once controller is running)

---

## Controller API (Pi-side) design

### Why an HTTP controller (instead of SSH)

- Works over Tailscale on restrictive networks.
- GUI-friendly (simple JSON calls).
- Allows safe policy: validate settings, reject invalid combos, implement “restart with args”, and expose structured status.

### Process control model

- Controller runs `edge/inference_pipeline.py` as a child process.
- All **setting changes** are applied as:
  1) stop current process (graceful),
  2) start new process with updated args,
  3) stream logs / profile lines.

This matches the edge runtime reality: key knobs are CLI flags (`--hops-per-io`, `--target-output-ahead-sec`, device IDs, etc.).

### Suggested endpoints

Implement these first (minimum viable surface area):

- `GET /health`
  - Returns `{ "ok": true }`

- `GET /status`
  - Returns:
    - `running`: bool
    - `pid`: int | null
    - `args`: current argv list / structured config
    - `started_at`: timestamp
    - `last_profile`: last parsed profile sample (if available)
    - `recent_errors`: recent stderr lines (ring buffer)

- `POST /start`
  - Body: `{ "config": { ... } }`
  - If already running: either reject or restart (prefer explicit `POST /restart`).

- `POST /stop`
  - Stops current pipeline process.

- `POST /restart`
  - Body: `{ "config": { ... } }`
  - Always restarts with new args.

- `GET /devices`
  - Returns the current input/output audio device list as seen by the Pi.
  - Purpose: avoid hardcoding device indices that may change across reboot or unplug/replug.
  - Implementation options:
    - use `sounddevice.query_devices()` in-process (preferred for speed), or
    - run `python -u edge/inference_pipeline.py --model <path> --list-devices` and return the text.

- `GET /logs?tail=200`
  - Returns last N lines from stdout/stderr ring.

- `GET /events` (optional)
  - Server-Sent Events (SSE): pushes log lines + parsed profile events to UI.

### Config schema (maps directly to existing CLI flags)

Minimal “must-have” fields:

- Model and contract:
  - `model`, `contract_path` (or zoo selection)
- Audio devices:
  - `input_device`, `output_device`
  - `input_sr`, `model_sr`, `output_sr`
  - `input_channels`, `output_channels`, `channel_map`
- Latency / buffering controls:
  - `hops_per_io`
  - `max_capture_blocks`
  - `target_output_ahead_sec`
  - `max_output_ahead_sec`
  - `queue_policy`
  - `overload_policy`
- Threads:
  - `intra_op_threads`, `inter_op_threads`
- Visibility:
  - `profile=true`, `profile_interval`, `monitor=false` (monitor prints can add jitter)

The controller should validate this schema against:

- `edge/INFERENCE_PIPELINE_ARGS.md` (flag meanings)
- runtime invariants (e.g., `max_output_ahead_sec > target_output_ahead_sec`, `max_capture_blocks >= 1`)
- path allowlists (do not allow arbitrary executables / files from the client)

---

## How the controller should parse “profile” output

`edge/inference_pipeline.py` already provides periodic summary lines under `--profile`.

Controller should:

- Capture stdout line-by-line.
- Run the child process with unbuffered output (`python -u ...`) so profile/log lines stream immediately.
- Detect profile lines by known stage keys (the profile output is stage-timing oriented).
- Parse into a structured dict that matches the actual profile output fields. Suggested target shape:

```json
{
  "capture_to_frame_ms": 0.0,
  "dc_ms": 0.0,
  "level_valid_ms": 0.0,
  "frame_to_infer_ms": 0.0,
  "infer_ms": 0.0,
  "post_out_ms": 0.0,
  "e2e_ms": 0.0,
  "drift_ppm": 0.0,
  "capture_queue_fill": 0,
  "capture_queue_capacity": 0,
  "drops_oldest": 0,
  "out_ahead_ms": 0.0,
  "out_dropped_frames": 0,
  "underrun_frames": 0,
  "model_ring_ms": 0.0,
  "model_ring_drops": 0,
  "model_ring_resets": 0
}
```

This allows the GUI to show real graphs and to warn if:

- `e2e_ms` or `infer_ms` spikes beyond what the current block budget can tolerate,
- `out_ahead_ms` is pinned at 0 (underrun risk) or constantly trimming (too-low ahead target),
- frequent `shed` mode / resets occur,
- capture is stale (large `capture_to_frame_ms`) or the queue is persistently full.

Note: `capture_to_frame_ms` is measured from the host input callback timestamp (not from the Pico’s I2S/DMA point).

---

## Security model

Key rule:

**Tailscale access is necessary, but not sufficient.** Use **Tailscale + token + firewall/ACL**.

Recommended bind/firewall options (in order):

1. Bind the API server only to the **Pi’s Tailscale IP** (or the `tailscale0` interface), if convenient.
2. If binding to `0.0.0.0`, firewall the API so only `tailscale0` can reach it.
3. Still require a **bearer token** (shared secret) for all non-`/health` endpoints.

Also:

- Add Tailscale **ACL** rule: only the laptop user/device can reach the controller port (e.g., `:8000`) on the Pi.
- Do not expose the API to the public Internet (no port forwarding, no public tunnel) unless explicitly required.

Implementation guardrails:

- Never construct the child command with `shell=True`.
- Build an argv list and pass it to `subprocess.Popen(argv, ...)`.
- Whitelist/allowlist `--model` and `--contract-path` locations; do not let the client run arbitrary files.
- Reject dangerous/invalid configs at minimum:
  - `max_output_ahead_sec <= target_output_ahead_sec`
  - `max_capture_blocks < 1`
  - `hops_per_io < 1`
  - `intra_op_threads < 1` or `inter_op_threads < 1`
  - `input_channels < len(channel_map)`
  - output gain outside a safe range
  - unknown/unreadable model or contract path

---

## systemd service (Pi)

Run the controller as a systemd service so it starts on boot and restarts on crash:

- `soundbubble-controller.service`
  - `After=network-online.target tailscaled.service`
  - `Restart=always`
  - Logs to journald

The controller then manages the pipeline process lifecycle itself.

Recommendation: use systemd only for the **controller**, not for every possible pipeline configuration. The controller owns pipeline lifecycle.

Example direction:

```ini
[Unit]
Description=Sound Bubble Controller
After=network-online.target tailscaled.service
Wants=network-online.target

[Service]
User=pi
WorkingDirectory=/home/pi/soundbubble
Environment=PYTHONUNBUFFERED=1
ExecStart=/home/pi/soundbubble/.venv/bin/python -m controller.app
Restart=always
RestartSec=2
SupplementaryGroups=audio

[Install]
WantedBy=multi-user.target
```

If real-time priority is introduced later, the service may also require:

- `LimitRTPRIO=95`
- `LimitMEMLOCK=infinity`
- `AmbientCapabilities=CAP_SYS_NICE`

---

## Laptop GUI plan

Two viable UI approaches:

### Option A — Web UI (recommended for speed)

- A small local page/app that calls the API.
- Advantages: fast iteration, easy charts (Plotly/Chart.js), works on any OS.

### Option B — PyQt GUI

- Simple settings form + preset buttons (e.g., “Stable demo”, “Low latency”).
- Live panel for:
  - `e2e_ms`, `infer_ms`, `capture_to_frame_ms`
  - `out_ahead_ms`, `underrun_frames`, `drops_oldest`
  - `model_ring_drops`, `model_ring_resets`

Both options use the same API and can run on the laptop while connected to any network (hotspot included).

---

## Suggested staged rollout

1. **Phase 1: connectivity**
   - Install Tailscale on Pi + laptop.
   - Verify laptop can reach Pi tailnet IP.

2. **Phase 2: minimal controller**
   - Implement `/health`, `/status`, `/start`, `/stop`, `/restart`, `/logs?tail=200`, and `/devices`.
   - Use restart-with-args only.

3. **Phase 3: observability**
   - Capture stdout/stderr ring buffer.
   - Parse `--profile` line into structured status.

4. **Phase 4: UI**
   - Add presets for latency tuning:
     - `hops_per_io=2`, `target_output_ahead=0.04` (stable)
     - `hops_per_io=1`, `max_capture_blocks=1`, `target_output_ahead=0.015` (low latency; may reduce wet coverage if infer is slow)

5. **Phase 5: safety + polish**
   - Token auth + Tailscale ACL.
   - “Are you sure?” guardrails for risky configs.

---

## Mapping to existing docs

- Runtime architecture and buffering: `edge/EDGE_RUNTIME.md`
- Full CLI flags: `edge/INFERENCE_PIPELINE_ARGS.md`
- Validation targets: `edge/VALIDATION_MATRIX.md`

