# Cursor Auto Mode implementation brief — Sound Bubble RPi remote controller over Tailscale

## What Cursor must build

Implement a **stdlib-only HTTP controller** for the existing `edge/inference_pipeline.py` runtime.

The controller will run on the Raspberry Pi as a `systemd` service. A laptop/PC connected to the same Tailscale tailnet will call the controller API using the Pi's Tailscale IP or MagicDNS name.

The controller must:

1. Start, stop, and restart `edge/inference_pipeline.py`.
2. Build subprocess commands using an argv list only. **Never use `shell=True`.**
3. Validate runtime config before launching the child process.
4. Capture stdout/stderr into a ring buffer.
5. Parse `[profile] ...` lines from `inference_pipeline.py` into structured JSON.
6. Provide `/health`, `/status`, `/start`, `/stop`, `/restart`, `/logs`, and `/devices` endpoints.
7. Require bearer-token auth for every endpoint except `/health`.
8. Keep the implementation dependency-light. Do **not** add FastAPI/uvicorn yet.

---

## Existing repo facts to preserve

The relevant existing runtime file is:

```text
edge/inference_pipeline.py
```

The CLI arguments are defined in `parse_args()` around lines 59-110. The controller config must map only to the CLI flags that actually exist there.

Important existing profile output is printed around lines 469-481 and currently looks like this:

```text
[profile] capture->frame=...ms  dc=...ms  level-valid=...ms  frame->infer=...ms  infer=...ms  post+out=...ms  e2e=...ms  drift=...ppm  q=.../... drops_oldest=...  out_ahead=...ms dropped=... underrun_frames=...  model_ring=...ms drops=... resets=...
```

Do **not** parse old/nonexistent fields such as `capture_age_ms`, `infer_mean_ms`, `infer_total_ms`, `steps`, or `mode`.

---

## Do not implement these yet

For this pass, do **not** implement:

- Laptop GUI
- SSE `/events`
- Web dashboard
- Public Internet tunnelling
- Remote desktop / VNC
- Hot-applying model parameters mid-stream

Use restart-with-new-args only.

---

## File changes overview

Create these new files:

```text
edge/controller/__init__.py
edge/controller/app.py
edge/controller/config.py
edge/controller/profile_parser.py
edge/controller/process_manager.py
edge/controller/server.py
edge/rpi5/soundbubble-controller.service
```

Optional doc update after implementation:

```text
edge/README_EDGE.md
```

No changes are required to `edge/inference_pipeline.py` for the first implementation, because the controller launches it with `python -u` and `PYTHONUNBUFFERED=1`.

---

## 1. Create `edge/controller/__init__.py`

```python
"""Remote controller package for the Sound Bubble edge runtime."""
```

---

## 2. Create `edge/controller/profile_parser.py`

```python
from __future__ import annotations

import re
from typing import Any

_PROFILE_RE = re.compile(
    r"^\[profile\]\s+"
    r"capture->frame=(?P<capture_to_frame_ms>[-+0-9.]+)ms\s+"
    r"dc=(?P<dc_ms>[-+0-9.]+)ms\s+"
    r"level-valid=(?P<level_valid_ms>[-+0-9.]+)ms\s+"
    r"frame->infer=(?P<frame_to_infer_ms>[-+0-9.]+)ms\s+"
    r"infer=(?P<infer_ms>[-+0-9.]+)ms\s+"
    r"post\+out=(?P<post_out_ms>[-+0-9.]+)ms\s+"
    r"e2e=(?P<e2e_ms>[-+0-9.]+)ms\s+"
    r"drift=(?P<drift_ppm>[-+0-9.]+)ppm\s+"
    r"q=(?P<capture_queue_fill>\d+)/(?:\s*)?(?P<capture_queue_capacity>\d+)\s+"
    r"drops_oldest=(?P<drops_oldest>\d+)\s+"
    r"out_ahead=(?P<out_ahead_ms>[-+0-9.]+)ms\s+"
    r"dropped=(?P<out_dropped_frames>\d+)\s+"
    r"underrun_frames=(?P<underrun_frames>\d+)\s+"
    r"model_ring=(?P<model_ring_ms>[-+0-9.]+)ms\s+"
    r"drops=(?P<model_ring_drops>\d+)\s+"
    r"resets=(?P<model_ring_resets>\d+)"
)

_FLOAT_KEYS = {
    "capture_to_frame_ms",
    "dc_ms",
    "level_valid_ms",
    "frame_to_infer_ms",
    "infer_ms",
    "post_out_ms",
    "e2e_ms",
    "drift_ppm",
    "out_ahead_ms",
    "model_ring_ms",
}

_INT_KEYS = {
    "capture_queue_fill",
    "capture_queue_capacity",
    "drops_oldest",
    "out_dropped_frames",
    "underrun_frames",
    "model_ring_drops",
    "model_ring_resets",
}


def parse_profile_line(line: str) -> dict[str, Any] | None:
    """Parse one `[profile] ...` line from edge/inference_pipeline.py.

    Returns None when the line is not a profile line or does not match the
    current expected format.
    """
    match = _PROFILE_RE.search(line.strip())
    if not match:
        return None

    parsed: dict[str, Any] = {}
    groups = match.groupdict()
    for key in _FLOAT_KEYS:
        parsed[key] = float(groups[key])
    for key in _INT_KEYS:
        parsed[key] = int(groups[key])
    return parsed
```

---

## 3. Create `edge/controller/config.py`

```python
from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
EDGE_ROOT = REPO_ROOT / "edge"


@dataclass(slots=True)
class PipelineConfig:
    # Model selection
    model: str | None = "edge/model.onnx"
    contract_path: str | None = "edge/model.runtime.json"
    zoo_dir: str | None = None
    bubble_radius: float | None = None

    # Audio devices/channels
    input_device: int | None = None
    output_device: int | None = None
    input_channels: int = 8
    output_channels: int = 2
    channel_map: str = "0,1,2,3,4,5"
    monitor_input_channel: int = 0

    # Rates/chunking
    input_sr: int = 44100
    model_sr: int = 24000
    output_sr: int = 48000
    model_chunk: int = 192
    model_pad: int = 96
    model_num_ch: int = 6
    hops_per_io: int = 2
    latency: str = "low"

    # Realtime guardrails
    max_capture_blocks: int = 2
    max_output_ahead_sec: float = 0.10
    target_output_ahead_sec: float = 0.04
    rt_fifo_priority: int = 0

    # Signal conditioning
    output_gain: float = 1.0
    mix_dry: float = 0.0
    input_gain_db: float = 0.0
    alpha: float = 0.9999
    silence_dbfs: float = -50.0
    silence_window_frames: int = 8
    fault_min_dbfs: float = -1.0
    fault_max_dbfs: float = 0.0

    # ONNX/runtime safety
    max_overrun_ms: float = 25.0
    intra_op_threads: int = 2
    inter_op_threads: int = 1
    model_ring_ms: float = 64.0
    max_steps_per_block: int = 4
    require_soxr: bool = False

    # Visibility
    profile: bool = True
    profile_interval: float = 1.0
    monitor: bool = False
    monitor_interval: float = 0.5

    @classmethod
    def from_dict(cls, data: dict[str, Any] | None) -> "PipelineConfig":
        if data is None:
            data = {}
        allowed = {field.name for field in cls.__dataclass_fields__.values()}  # type: ignore[attr-defined]
        unknown = sorted(set(data) - allowed)
        if unknown:
            raise ValueError(f"Unknown config key(s): {', '.join(unknown)}")
        return cls(**data)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def _resolve_under_edge_or_repo(path_text: str | None, *, must_exist: bool, kind: str) -> str | None:
    if path_text is None:
        return None
    p = Path(path_text).expanduser()
    if not p.is_absolute():
        p = REPO_ROOT / p
    p = p.resolve()

    allowed_roots = [EDGE_ROOT.resolve()]
    if not any(p == root or root in p.parents for root in allowed_roots):
        raise ValueError(f"{kind} must be inside {EDGE_ROOT}; got {p}")
    if must_exist and not p.exists():
        raise ValueError(f"{kind} does not exist: {p}")
    return str(p)


def _parse_channel_map(channel_map: str) -> list[int]:
    values: list[int] = []
    for token in channel_map.split(","):
        token = token.strip()
        if not token:
            continue
        try:
            values.append(int(token))
        except ValueError as exc:
            raise ValueError("channel_map must be comma-separated integers, e.g. 0,1,2,3,4,5") from exc
    if not values:
        raise ValueError("channel_map cannot be empty")
    return values


def validate_config(config: PipelineConfig) -> PipelineConfig:
    if config.model is None and config.zoo_dir is None:
        raise ValueError("Provide either model or zoo_dir + bubble_radius")
    if config.zoo_dir is not None and config.bubble_radius is None:
        raise ValueError("zoo_dir requires bubble_radius")
    if config.latency not in {"low", "high"}:
        raise ValueError("latency must be 'low' or 'high'")

    ints_positive = {
        "input_channels": config.input_channels,
        "output_channels": config.output_channels,
        "input_sr": config.input_sr,
        "model_sr": config.model_sr,
        "output_sr": config.output_sr,
        "model_chunk": config.model_chunk,
        "model_pad": config.model_pad,
        "model_num_ch": config.model_num_ch,
        "hops_per_io": config.hops_per_io,
        "max_capture_blocks": config.max_capture_blocks,
        "intra_op_threads": config.intra_op_threads,
        "inter_op_threads": config.inter_op_threads,
        "silence_window_frames": config.silence_window_frames,
        "max_steps_per_block": config.max_steps_per_block,
    }
    bad = [name for name, value in ints_positive.items() if int(value) < 1]
    if bad:
        raise ValueError(f"These integer fields must be >= 1: {', '.join(bad)}")

    if config.input_device is not None and config.input_device < 0:
        raise ValueError("input_device must be >= 0 when provided")
    if config.output_device is not None and config.output_device < 0:
        raise ValueError("output_device must be >= 0 when provided")
    if config.monitor_input_channel < 0 or config.monitor_input_channel >= config.input_channels:
        raise ValueError("monitor_input_channel must be within input_channels")

    channel_map = _parse_channel_map(config.channel_map)
    if len(channel_map) > config.model_num_ch:
        raise ValueError("channel_map selects more channels than model_num_ch")
    if max(channel_map) >= config.input_channels or min(channel_map) < 0:
        raise ValueError("channel_map contains an index outside input_channels")

    if config.max_output_ahead_sec <= config.target_output_ahead_sec:
        raise ValueError("max_output_ahead_sec must be greater than target_output_ahead_sec")
    if config.target_output_ahead_sec <= 0:
        raise ValueError("target_output_ahead_sec must be > 0")
    if config.model_ring_ms <= 0:
        raise ValueError("model_ring_ms must be > 0")
    if config.max_overrun_ms <= 0:
        raise ValueError("max_overrun_ms must be > 0")
    if config.profile_interval <= 0 or config.monitor_interval <= 0:
        raise ValueError("profile_interval and monitor_interval must be > 0")
    if not (0.0 <= config.mix_dry <= 1.0):
        raise ValueError("mix_dry must be in [0.0, 1.0]")
    if not (0.0 <= config.output_gain <= 2.0):
        raise ValueError("output_gain must be in [0.0, 2.0] for safety")

    config.model = _resolve_under_edge_or_repo(config.model, must_exist=True, kind="model")
    config.contract_path = _resolve_under_edge_or_repo(config.contract_path, must_exist=True, kind="contract_path")
    config.zoo_dir = _resolve_under_edge_or_repo(config.zoo_dir, must_exist=True, kind="zoo_dir")
    return config


def build_pipeline_argv(config: PipelineConfig) -> list[str]:
    cfg = validate_config(config)
    script = EDGE_ROOT / "inference_pipeline.py"
    argv = [str(script)]

    value_flags: list[tuple[str, Any]] = [
        ("--model", cfg.model),
        ("--contract-path", cfg.contract_path),
        ("--zoo-dir", cfg.zoo_dir),
        ("--bubble-radius", cfg.bubble_radius),
        ("--input-device", cfg.input_device),
        ("--output-device", cfg.output_device),
        ("--input-channels", cfg.input_channels),
        ("--output-channels", cfg.output_channels),
        ("--channel-map", cfg.channel_map),
        ("--monitor-input-channel", cfg.monitor_input_channel),
        ("--input-sr", cfg.input_sr),
        ("--model-sr", cfg.model_sr),
        ("--output-sr", cfg.output_sr),
        ("--model-chunk", cfg.model_chunk),
        ("--model-pad", cfg.model_pad),
        ("--model-num-ch", cfg.model_num_ch),
        ("--hops-per-io", cfg.hops_per_io),
        ("--latency", cfg.latency),
        ("--max-capture-blocks", cfg.max_capture_blocks),
        ("--max-output-ahead-sec", cfg.max_output_ahead_sec),
        ("--target-output-ahead-sec", cfg.target_output_ahead_sec),
        ("--rt-fifo-priority", cfg.rt_fifo_priority),
        ("--output-gain", cfg.output_gain),
        ("--mix-dry", cfg.mix_dry),
        ("--input-gain-db", cfg.input_gain_db),
        ("--alpha", cfg.alpha),
        ("--silence-dbfs", cfg.silence_dbfs),
        ("--silence-window-frames", cfg.silence_window_frames),
        ("--fault-min-dbfs", cfg.fault_min_dbfs),
        ("--fault-max-dbfs", cfg.fault_max_dbfs),
        ("--max-overrun-ms", cfg.max_overrun_ms),
        ("--intra-op-threads", cfg.intra_op_threads),
        ("--inter-op-threads", cfg.inter_op_threads),
        ("--profile-interval", cfg.profile_interval),
        ("--monitor-interval", cfg.monitor_interval),
        ("--model-ring-ms", cfg.model_ring_ms),
        ("--max-steps-per-block", cfg.max_steps_per_block),
    ]
    for flag, value in value_flags:
        if value is not None:
            argv.extend([flag, str(value)])

    if cfg.profile:
        argv.append("--profile")
    if cfg.monitor:
        argv.append("--monitor")
    if cfg.require_soxr:
        argv.append("--require-soxr")
    return argv
```

---

## 4. Create `edge/controller/process_manager.py`

```python
from __future__ import annotations

from collections import deque
from datetime import datetime, timezone
import os
from pathlib import Path
import subprocess
import sys
import threading
from typing import Any

from .config import PipelineConfig, REPO_ROOT, build_pipeline_argv, validate_config
from .profile_parser import parse_profile_line


class ProcessManager:
    def __init__(self, *, log_capacity: int = 5000, error_capacity: int = 300) -> None:
        self._lock = threading.RLock()
        self._process: subprocess.Popen[str] | None = None
        self._started_at: str | None = None
        self._current_config: dict[str, Any] | None = None
        self._current_argv: list[str] | None = None
        self._last_returncode: int | None = None
        self._logs: deque[str] = deque(maxlen=log_capacity)
        self._errors: deque[str] = deque(maxlen=error_capacity)
        self._last_profile: dict[str, Any] | None = None

    def is_running(self) -> bool:
        with self._lock:
            return self._process is not None and self._process.poll() is None

    def start(self, config_data: dict[str, Any] | None) -> dict[str, Any]:
        with self._lock:
            if self.is_running():
                raise RuntimeError("Pipeline is already running. Use /restart to replace it.")

            config = validate_config(PipelineConfig.from_dict(config_data))
            pipeline_argv = build_pipeline_argv(config)
            argv = [sys.executable, "-u", *pipeline_argv]

            env = os.environ.copy()
            env["PYTHONUNBUFFERED"] = "1"
            env.setdefault("PYTHONPATH", str(REPO_ROOT))

            self._logs.clear()
            self._errors.clear()
            self._last_profile = None
            self._last_returncode = None

            process = subprocess.Popen(
                argv,
                cwd=str(REPO_ROOT),
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
                universal_newlines=True,
            )
            self._process = process
            self._started_at = datetime.now(timezone.utc).isoformat()
            self._current_config = config.to_dict()
            self._current_argv = argv

            if process.stdout is not None:
                threading.Thread(
                    target=self._read_stream,
                    args=(process.stdout, "stdout"),
                    name="soundbubble-stdout-reader",
                    daemon=True,
                ).start()
            if process.stderr is not None:
                threading.Thread(
                    target=self._read_stream,
                    args=(process.stderr, "stderr"),
                    name="soundbubble-stderr-reader",
                    daemon=True,
                ).start()
            return self.status()

    def stop(self) -> dict[str, Any]:
        with self._lock:
            process = self._process
            if process is None:
                return self.status()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    self._append_log("[controller] process did not terminate within 5s; killing", "stderr")
                    process.kill()
                    process.wait(timeout=5.0)
            self._last_returncode = process.returncode
            self._process = None
            return self.status()

    def restart(self, config_data: dict[str, Any] | None) -> dict[str, Any]:
        self.stop()
        return self.start(config_data)

    def status(self) -> dict[str, Any]:
        with self._lock:
            pid: int | None = None
            running = False
            returncode = self._last_returncode
            if self._process is not None:
                pid = self._process.pid
                polled = self._process.poll()
                running = polled is None
                if polled is not None:
                    returncode = polled
                    self._last_returncode = polled
                    self._process = None
                    pid = None
            return {
                "running": running,
                "pid": pid,
                "returncode": returncode,
                "started_at": self._started_at,
                "args": self._current_argv,
                "config": self._current_config,
                "last_profile": self._last_profile,
                "recent_errors": list(self._errors)[-50:],
            }

    def logs(self, tail: int = 200) -> dict[str, Any]:
        tail = max(1, min(int(tail), self._logs.maxlen or 5000))
        with self._lock:
            return {"lines": list(self._logs)[-tail:]}

    def _read_stream(self, stream, stream_name: str) -> None:
        try:
            for line in iter(stream.readline, ""):
                self._append_log(line.rstrip("\n"), stream_name)
        finally:
            try:
                stream.close()
            except Exception:
                pass

    def _append_log(self, line: str, stream_name: str) -> None:
        stamped = f"[{stream_name}] {line}"
        with self._lock:
            self._logs.append(stamped)
            if stream_name == "stderr" or "error" in line.lower() or "exception" in line.lower():
                self._errors.append(stamped)
            parsed = parse_profile_line(line)
            if parsed is not None:
                parsed["received_at"] = datetime.now(timezone.utc).isoformat()
                self._last_profile = parsed
```

---

## 5. Create `edge/controller/server.py`

```python
from __future__ import annotations

import argparse
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse

from .process_manager import ProcessManager


def query_audio_devices() -> dict[str, Any]:
    try:
        import sounddevice as sd
    except ImportError as exc:
        raise RuntimeError("sounddevice is not installed in this environment") from exc

    devices = sd.query_devices()
    hostapis = sd.query_hostapis()
    result = []
    for index, device in enumerate(devices):
        hostapi_name = hostapis[device["hostapi"]]["name"]
        result.append(
            {
                "index": index,
                "name": device["name"],
                "hostapi": hostapi_name,
                "max_input_channels": int(device["max_input_channels"]),
                "max_output_channels": int(device["max_output_channels"]),
                "default_samplerate": float(device["default_samplerate"]),
            }
        )
    return {"devices": result}


class ControllerServer(ThreadingHTTPServer):
    def __init__(self, server_address, RequestHandlerClass, *, manager: ProcessManager, token: str | None):
        super().__init__(server_address, RequestHandlerClass)
        self.manager = manager
        self.token = token


class Handler(BaseHTTPRequestHandler):
    server: ControllerServer

    def log_message(self, fmt: str, *args) -> None:  # keep stdout clean; journald still captures process logs
        print(f"[controller-http] {self.address_string()} - {fmt % args}")

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        try:
            if parsed.path == "/health":
                self._json(200, {"ok": True})
                return
            self._require_auth()
            if parsed.path == "/status":
                self._json(200, self.server.manager.status())
            elif parsed.path == "/logs":
                qs = parse_qs(parsed.query)
                tail = int(qs.get("tail", ["200"])[0])
                self._json(200, self.server.manager.logs(tail=tail))
            elif parsed.path == "/devices":
                self._json(200, query_audio_devices())
            else:
                self._json(404, {"error": "not found"})
        except Exception as exc:
            self._json(400, {"error": str(exc)})

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        try:
            self._require_auth()
            body = self._read_json_body()
            config = body.get("config") if isinstance(body, dict) else None
            if parsed.path == "/start":
                self._json(200, self.server.manager.start(config))
            elif parsed.path == "/stop":
                self._json(200, self.server.manager.stop())
            elif parsed.path == "/restart":
                self._json(200, self.server.manager.restart(config))
            else:
                self._json(404, {"error": "not found"})
        except Exception as exc:
            self._json(400, {"error": str(exc)})

    def _require_auth(self) -> None:
        token = self.server.token
        if token is None:
            return
        expected = f"Bearer {token}"
        actual = self.headers.get("Authorization", "")
        if actual != expected:
            raise PermissionError("missing or invalid bearer token")

    def _read_json_body(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length == 0:
            return {}
        raw = self.rfile.read(length)
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))

    def _json(self, status: int, payload: dict[str, Any]) -> None:
        data = json.dumps(payload, indent=2, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def serve(host: str, port: int, token: str | None) -> None:
    manager = ProcessManager()
    httpd = ControllerServer((host, port), Handler, manager=manager, token=token)
    print(f"[controller] listening on http://{host}:{port}")
    if token is None:
        print("[controller-warning] bearer token disabled; only use this for local testing")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("[controller] stopping")
    finally:
        manager.stop()
        httpd.server_close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Sound Bubble remote controller HTTP API")
    parser.add_argument("--host", default=os.environ.get("SOUND_BUBBLE_CONTROLLER_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("SOUND_BUBBLE_CONTROLLER_PORT", "8000")))
    parser.add_argument("--allow-no-token", action="store_true", help="Allow unauthenticated non-health endpoints. Use only for local testing.")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    token = os.environ.get("SOUND_BUBBLE_CONTROLLER_TOKEN")
    if not token and not args.allow_no_token:
        raise SystemExit("Set SOUND_BUBBLE_CONTROLLER_TOKEN or pass --allow-no-token for local testing.")
    serve(args.host, args.port, token)
```

---

## 6. Create `edge/controller/app.py`

```python
from __future__ import annotations

from .server import main


if __name__ == "__main__":
    main()
```

---

## 7. Create `edge/rpi5/soundbubble-controller.service`

```ini
[Unit]
Description=Sound Bubble Remote Controller
After=network-online.target tailscaled.service
Wants=network-online.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/soundbubble
Environment=PYTHONUNBUFFERED=1
EnvironmentFile=/etc/soundbubble-controller.env
ExecStart=/home/pi/soundbubble/.venv/bin/python -m edge.controller.app --host 0.0.0.0 --port 8000
Restart=always
RestartSec=2
SupplementaryGroups=audio

[Install]
WantedBy=multi-user.target
```

---

## 8. Optional README update

Append this section to `edge/README_EDGE.md`:

```markdown
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

```
---

## 9. Local test commands for Cursor

Run these from the repository root.

### Syntax check

```bash
python -m py_compile \
  edge/controller/profile_parser.py \
  edge/controller/config.py \
  edge/controller/process_manager.py \
  edge/controller/server.py \
  edge/controller/app.py
```

### Parser smoke test

```bash
python - <<'PY'
from edge.controller.profile_parser import parse_profile_line

line = '[profile] capture->frame=1.23ms  dc=0.01ms  level-valid=0.02ms  frame->infer=3.45ms  infer=9.87ms  post+out=0.20ms  e2e=12.34ms  drift=+0.00ppm  q=1/2 drops_oldest=0  out_ahead=40.0ms dropped=0 underrun_frames=0  model_ring=8.0ms drops=0 resets=0'
parsed = parse_profile_line(line)
assert parsed is not None
assert parsed['capture_to_frame_ms'] == 1.23
assert parsed['level_valid_ms'] == 0.02
assert parsed['infer_ms'] == 9.87
assert parsed['out_ahead_ms'] == 40.0
assert parsed['capture_queue_fill'] == 1
assert parsed['capture_queue_capacity'] == 2
assert parsed['model_ring_resets'] == 0
print(parsed)
PY
```

### Config validation smoke test

```bash
python - <<'PY'
from edge.controller.config import PipelineConfig, build_pipeline_argv

cfg = PipelineConfig(
    model='edge/model.onnx',
    contract_path='edge/model.runtime.json',
    input_device=1,
    output_device=2,
    profile=True,
    monitor=False,
)
argv = build_pipeline_argv(cfg)
print(argv)
assert '--model' in argv
assert '--contract-path' in argv
assert '--profile' in argv
assert '--monitor' not in argv
PY
```

### Run controller locally

Terminal 1:

```bash
export SOUND_BUBBLE_CONTROLLER_TOKEN='dev-token-change-me'
python -m edge.controller.app --host 127.0.0.1 --port 8000
```

Terminal 2:

```bash
curl http://127.0.0.1:8000/health
curl -H "Authorization: Bearer dev-token-change-me" http://127.0.0.1:8000/status
curl -H "Authorization: Bearer dev-token-change-me" http://127.0.0.1:8000/devices
```

The `/devices` endpoint requires `sounddevice` and a working audio stack. It is acceptable for it to return a JSON error on a non-Pi development machine without PortAudio/audio devices.

---

## 10. Start/stop API examples

Replace `100.x.y.z` with the Pi's Tailscale IP or MagicDNS name.

### Start pipeline

```bash
TOKEN='replace-with-real-token'
PI='100.x.y.z'

curl -X POST "http://$PI:8000/start" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "config": {
      "model": "edge/model.onnx",
      "contract_path": "edge/model.runtime.json",
      "input_device": 1,
      "output_device": 2,
      "input_channels": 8,
      "output_channels": 2,
      "channel_map": "0,1,2,3,4,5",
      "hops_per_io": 2,
      "max_capture_blocks": 2,
      "target_output_ahead_sec": 0.04,
      "max_output_ahead_sec": 0.10,
      "profile": true,
      "profile_interval": 1.0,
      "monitor": false
    }
  }'
```

### Status

```bash
curl -H "Authorization: Bearer $TOKEN" "http://$PI:8000/status"
```

### Logs

```bash
curl -H "Authorization: Bearer $TOKEN" "http://$PI:8000/logs?tail=200"
```

### Restart with lower-latency preset

```bash
curl -X POST "http://$PI:8000/restart" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "config": {
      "model": "edge/model.onnx",
      "contract_path": "edge/model.runtime.json",
      "input_device": 1,
      "output_device": 2,
      "input_channels": 8,
      "output_channels": 2,
      "channel_map": "0,1,2,3,4,5",
      "hops_per_io": 1,
      "max_capture_blocks": 1,
      "target_output_ahead_sec": 0.015,
      "max_output_ahead_sec": 0.06,
      "profile": true,
      "profile_interval": 0.5,
      "monitor": false
    }
  }'
```

### Stop

```bash
curl -X POST "http://$PI:8000/stop" \
  -H "Authorization: Bearer $TOKEN"
```

---

## 11. Pi deployment steps

Assume the repo is at:

```text
/home/pi/soundbubble
```

If the repo path is different, update `WorkingDirectory` and `ExecStart` in `edge/rpi5/soundbubble-controller.service`.

### Install Tailscale on Pi and laptop

On the Pi:

```bash
sudo tailscale up
 tailscale ip -4
```

On the laptop, sign in to the same tailnet and verify:

```bash
tailscale ping <pi-tailscale-ip-or-name>
```

### Create controller token env file on Pi

```bash
TOKEN="$(openssl rand -hex 32)"
echo "SOUND_BUBBLE_CONTROLLER_TOKEN=$TOKEN" | sudo tee /etc/soundbubble-controller.env
sudo chmod 600 /etc/soundbubble-controller.env
cat /etc/soundbubble-controller.env
```

Copy the token to the laptop password manager or `.env` file used by the GUI/client.

### Install service

```bash
sudo cp /home/pi/soundbubble/edge/rpi5/soundbubble-controller.service /etc/systemd/system/soundbubble-controller.service
sudo systemctl daemon-reload
sudo systemctl enable --now soundbubble-controller.service
sudo systemctl status soundbubble-controller.service
```

### View logs

```bash
journalctl -u soundbubble-controller.service -f
```

---

## 12. Firewall hardening

Use Tailscale plus bearer-token auth. If using `ufw`, allow port `8000` only on `tailscale0`:

```bash
sudo ufw allow OpenSSH
sudo ufw insert 1 allow in on tailscale0 to any port 8000 proto tcp
sudo ufw deny 8000/tcp
sudo ufw enable
sudo ufw status numbered
```

Then test from the laptop through the Pi's Tailscale IP:

```bash
curl http://<pi-tailscale-ip>:8000/health
curl -H "Authorization: Bearer <token>" http://<pi-tailscale-ip>:8000/status
```

Do **not** port-forward `8000` from a router. Do **not** expose this controller to the public Internet.

---

## 13. Tailscale ACL recommendation

In the Tailscale admin console, restrict access so only your laptop/user can reach the Pi controller port.

Example shape:

```json
{
  "acls": [
    {
      "action": "accept",
      "src": ["autogroup:member"],
      "dst": ["<pi-device-or-tag>:8000"]
    }
  ]
}
```

Adjust `<pi-device-or-tag>` to match your actual Tailscale device/tag policy.

---

## 14. Acceptance criteria

Cursor is done only when all of the following are true:

1. `python -m py_compile edge/controller/*.py` passes.
2. `python -m edge.controller.app --host 127.0.0.1 --port 8000` starts when `SOUND_BUBBLE_CONTROLLER_TOKEN` is set.
3. `GET /health` works without auth.
4. `GET /status` rejects requests without `Authorization: Bearer <token>`.
5. `GET /status` accepts requests with the correct token.
6. `/start` launches `edge/inference_pipeline.py` using `python -u` and an argv list.
7. No code path uses `shell=True`.
8. `/logs?tail=200` returns captured stdout/stderr lines.
9. `/status` includes `last_profile` after profile lines appear.
10. `/stop` terminates the child process gracefully and kills it only after timeout.
11. `/devices` returns JSON-formatted audio device info or a JSON error if `sounddevice`/PortAudio is unavailable.
12. `edge/rpi5/soundbubble-controller.service` exists and uses `EnvironmentFile=/etc/soundbubble-controller.env`.

---

## 15. Known limitations after this pass

This pass deliberately does not include a polished GUI. Use `curl` first. After the controller is stable, the laptop GUI can call the same API.

`GET /events` is not implemented yet. Use polling:

```text
/status every 0.5-1.0 s
/logs?tail=200 on demand
```

The controller only validates config shape and safety invariants. It does not guarantee that a selected audio device supports the requested sample rate; the existing `inference_pipeline.py` preflight remains the source of truth for actual audio-device compatibility.

---

## 16. Final implementation principle

The Pi should remain the only machine doing real-time audio inference. The laptop is only a remote control surface.

Correct mental model:

```text
Laptop GUI / curl
    ↓ Tailscale private HTTP
Pi controller service
    ↓ subprocess argv, no shell
edge/inference_pipeline.py
    ↓ local audio devices + ONNX runtime
Sound Bubble live audio
```
