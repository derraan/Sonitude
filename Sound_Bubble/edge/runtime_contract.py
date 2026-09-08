from __future__ import annotations

import json
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
    )
