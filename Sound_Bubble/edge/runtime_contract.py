from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class RuntimeContract:
    model_sr: int
    model_num_ch: int
    model_chunk: int
    model_pad: int
    frame_len: int
    expected_input_channels: int
    expected_channel_map: list[int]
    notes: str
    # Provenance (optional; older contracts won't have these).
    model_class: Optional[str] = None
    dis_threshold: Optional[float] = None
    # Multi-distance models expose a dis_embed input at inference time; this lists
    # the one-hot-encoded radii the model was trained on. When None, the model is
    # single-distance (baked into weights) and `dis_threshold` is the authority.
    supported_radii: Optional[list[float]] = None
    source_config: Optional[str] = None
    source_run_dir: Optional[str] = None
    source_checkpoint: Optional[str] = None
    trained_epochs: Optional[int] = None
    distance_head_enabled: Optional[bool] = None
    distance_head_trained: Optional[bool] = None
    distance_valid_threshold: Optional[float] = None
    distance_update_interval_s: Optional[float] = None
    distance_history_tau_s: Optional[float] = None
    count_head_enabled: Optional[bool] = None
    count_head_trained: Optional[bool] = None
    count_classes: Optional[list[str]] = None
    count_min_confidence: Optional[float] = None
    count_history_tau_s: Optional[float] = None


def from_training_params(
    params: dict,
    frame_len: int | None = None,
    source_config: str | None = None,
    source_run_dir: str | None = None,
    source_checkpoint: str | None = None,
    trained_epochs: int | None = None,
    supported_radii: list[float] | None = None,
) -> RuntimeContract:
    model_params = params["pl_module_args"]["model_params"]
    model_sr = int(params["pl_module_args"].get("sr", 24000))
    model_num_ch = int(model_params["num_ch"])
    model_chunk = int(model_params["stft_chunk_size"])
    model_pad = int(model_params["stft_pad_size"])
    resolved_frame_len = int(frame_len or (model_chunk + model_pad))
    model_class = str(params["pl_module_args"].get("model") or "") or None
    dis_threshold_raw = (params.get("train_data_args") or {}).get("dis_threshold")
    dis_threshold = float(dis_threshold_raw) if dis_threshold_raw is not None else None
    distance_cfg = (model_params.get("distance_head") or {})
    distance_head_enabled = bool(distance_cfg.get("enabled", False))
    distance_update_interval_s = model_chunk / max(1, model_sr)
    distance_history_tau_s = float(distance_cfg.get("ema_tau_s", 1.0)) if distance_head_enabled else None
    distance_valid_threshold = float(distance_cfg.get("valid_threshold", 0.5)) if distance_head_enabled else None
    has_ckpt = bool(source_checkpoint and os.path.isfile(source_checkpoint))
    distance_head_trained = bool(
        distance_head_enabled and has_ckpt and trained_epochs is not None and int(trained_epochs) >= 0
    )
    count_cfg = (model_params.get("count_head") or {})
    count_head_enabled = bool(count_cfg.get("enabled", False))
    count_head_trained = bool(
        count_head_enabled and has_ckpt and trained_epochs is not None and int(trained_epochs) >= 0
    )
    count_min_confidence = float(count_cfg.get("min_confidence", 0.6)) if count_head_enabled else None
    count_history_tau_s = float(count_cfg.get("ema_tau_s", 0.5)) if count_head_enabled else None
    return RuntimeContract(
        model_sr=model_sr,
        model_num_ch=model_num_ch,
        model_chunk=model_chunk,
        model_pad=model_pad,
        frame_len=resolved_frame_len,
        expected_input_channels=8,
        expected_channel_map=list(range(model_num_ch)),
        notes="Auto-generated from experiment config used during ONNX export.",
        model_class=model_class,
        dis_threshold=dis_threshold,
        supported_radii=list(supported_radii) if supported_radii else None,
        source_config=source_config,
        source_run_dir=source_run_dir,
        source_checkpoint=source_checkpoint,
        trained_epochs=trained_epochs,
        distance_head_enabled=distance_head_enabled,
        distance_head_trained=distance_head_trained if distance_head_enabled else None,
        distance_valid_threshold=distance_valid_threshold,
        distance_update_interval_s=distance_update_interval_s if distance_head_enabled else None,
        distance_history_tau_s=distance_history_tau_s,
        count_head_enabled=count_head_enabled,
        count_head_trained=count_head_trained if count_head_enabled else None,
        count_classes=["0", "1", "2+"] if count_head_enabled else None,
        count_min_confidence=count_min_confidence,
        count_history_tau_s=count_history_tau_s,
    )


def save_contract(path: str | Path, contract: RuntimeContract) -> None:
    path = Path(path)
    payload = asdict(contract)
    # Drop None-valued provenance so old readers see a minimal, clean file.
    payload = {k: v for k, v in payload.items() if v is not None}
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def load_contract(path: str | Path) -> RuntimeContract:
    path = Path(path)
    payload = json.loads(path.read_text(encoding="utf-8"))
    dis = payload.get("dis_threshold")
    ep = payload.get("trained_epochs")
    radii = payload.get("supported_radii")
    return RuntimeContract(
        model_sr=int(payload["model_sr"]),
        model_num_ch=int(payload["model_num_ch"]),
        model_chunk=int(payload["model_chunk"]),
        model_pad=int(payload["model_pad"]),
        frame_len=int(payload["frame_len"]),
        expected_input_channels=int(payload["expected_input_channels"]),
        expected_channel_map=[int(v) for v in payload["expected_channel_map"]],
        notes=str(payload.get("notes", "")),
        model_class=payload.get("model_class"),
        dis_threshold=float(dis) if dis is not None else None,
        supported_radii=[float(r) for r in radii] if radii else None,
        source_config=payload.get("source_config"),
        source_run_dir=payload.get("source_run_dir"),
        source_checkpoint=payload.get("source_checkpoint"),
        trained_epochs=int(ep) if ep is not None else None,
        distance_head_enabled=payload.get("distance_head_enabled"),
        distance_head_trained=payload.get("distance_head_trained"),
        distance_valid_threshold=float(payload["distance_valid_threshold"]) if payload.get("distance_valid_threshold") is not None else None,
        distance_update_interval_s=float(payload["distance_update_interval_s"]) if payload.get("distance_update_interval_s") is not None else None,
        distance_history_tau_s=float(payload["distance_history_tau_s"]) if payload.get("distance_history_tau_s") is not None else None,
        count_head_enabled=payload.get("count_head_enabled"),
        count_head_trained=payload.get("count_head_trained"),
        count_classes=[str(x) for x in payload.get("count_classes", [])] if payload.get("count_classes") is not None else None,
        count_min_confidence=float(payload["count_min_confidence"]) if payload.get("count_min_confidence") is not None else None,
        count_history_tau_s=float(payload["count_history_tau_s"]) if payload.get("count_history_tau_s") is not None else None,
    )
