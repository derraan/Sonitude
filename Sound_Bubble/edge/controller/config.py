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

