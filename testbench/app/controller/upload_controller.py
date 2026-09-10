"""Helpers for committing testbench settings into runtime YAML."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from shutil import copy2
from typing import Any

import yaml

from app.config_reader import DEFAULT_CONFIG_PATH
from app.processing.suppression import SuppressionMode, parse_suppression_mode
from app.storage.models import BinauralRequest, SuppressorRequest

__all__ = [
    "CalibrationCommitSnapshot",
    "DspCommitSnapshot",
    "UploadCommitResult",
    "apply_dsp_fields",
    "commit_runtime_config",
    "patch_runtime_yaml_dsp",
]

_UPLOADED_CALIBRATION_NAME = "calibration_uploaded.yaml"


@dataclass(frozen=True)
class CalibrationCommitSnapshot:
    variant: str
    variant_yaml: Path | None
    common_eq_enabled: bool
    common_eq_sections: list[dict[str, Any]] | None
    spatial_backend: str | None = None
    spatial_profile_path: Path | None = None


@dataclass(frozen=True)
class DspCommitSnapshot:
    suppression_mode: str
    suppression_backend: str | None
    suppressor: SuppressorRequest
    binaural: BinauralRequest
    spatial_backend: str | None = None
    spatial_profile_path: Path | None = None
    source_distance_m: float = 0.45


@dataclass(frozen=True)
class UploadCommitResult:
    committed_config_path: Path
    calibration_copy_path: Path
    backup_path: Path
    resolved_suppression_enabled: bool
    suppression_mode: SuppressionMode
    spatial_backend: str | None
    spatial_profile_path: Path | None


def _timestamp_backup_name(config_path: Path) -> str:
    stem = config_path.stem
    suffix = config_path.suffix or ".yaml"
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    return f"{stem}_{ts}{suffix}"


def _next_backup_path(config_path: Path) -> Path:
    backup_dir = config_path.parent / "backups"
    backup_dir.mkdir(parents=True, exist_ok=True)
    candidate = backup_dir / _timestamp_backup_name(config_path)
    if not candidate.exists():
        return candidate
    for idx in range(1, 1000):
        with_suffix = backup_dir / f"{candidate.stem}_{idx}{candidate.suffix}"
        if not with_suffix.exists():
            return with_suffix
    raise RuntimeError("Could not allocate unique backup filename in config/backups.")


def _normalize_common_eq_sections(sections: list[dict[str, Any]]) -> list[dict[str, Any]]:
    normalized: list[dict[str, Any]] = []
    for section in sections:
        normalized.append(
            {
                "type": str(section.get("type", section.get("ftype", "PK"))),
                "freq_hz": float(section["freq_hz"]),
                "gain_db": float(section.get("gain_db", 0.0)),
                "q": float(section["q"]),
            }
        )
    return normalized


def apply_dsp_fields(raw: dict[str, Any], dsp: DspCommitSnapshot) -> None:
    """Write RT DSP knobs into a runtime YAML mapping. Always forces near-field MVDR."""
    suppression = raw.setdefault("suppression", {})
    if not isinstance(suppression, dict):
        raise RuntimeError("Runtime config field 'suppression' must be a mapping.")
    mode = parse_suppression_mode(dsp.suppression_mode)
    yaml_enabled = bool(suppression.get("enabled", False))
    if mode is SuppressionMode.ON:
        suppression["enabled"] = True
    elif mode is SuppressionMode.OFF:
        suppression["enabled"] = False
    else:
        suppression["enabled"] = yaml_enabled

    if dsp.suppression_backend:
        suppression["backend"] = dsp.suppression_backend
    suppression["fade_ms"] = float(dsp.suppressor.fade_ms)
    suppression["activity_threshold"] = float(dsp.suppressor.activity_threshold)
    suppression["confidence_threshold"] = float(dsp.suppressor.confidence_threshold)

    spectral = suppression.setdefault("spectral", {})
    if not isinstance(spectral, dict):
        raise RuntimeError("Runtime config field 'suppression.spectral' must be a mapping.")
    spectral["gain_floor_db"] = float(dsp.suppressor.spectral_gain_floor_db)
    spectral["amplitude_range_bias"] = bool(dsp.suppressor.amplitude_range_bias)
    spectral["speech_low_hz"] = float(dsp.suppressor.speech_low_hz)
    spectral["speech_high_hz"] = float(dsp.suppressor.speech_high_hz)
    spectral["near_dominance_ratio"] = float(dsp.suppressor.near_dominance_ratio)

    steering = raw.setdefault("steering", {})
    if not isinstance(steering, dict):
        raise RuntimeError("Runtime config field 'steering' must be a mapping.")
    steering["model"] = "near_field"
    steering["source_distance_m"] = float(dsp.source_distance_m)
    steering["ambient_floor_linear"] = float(dsp.suppressor.ambient_floor_linear)

    binaural = raw.setdefault("binaural", {})
    if not isinstance(binaural, dict):
        raise RuntimeError("Runtime config field 'binaural' must be a mapping.")
    direction = binaural.setdefault("direction", {})
    if not isinstance(direction, dict):
        raise RuntimeError("Runtime config field 'binaural.direction' must be a mapping.")
    binaural["enabled"] = bool(dsp.binaural.enabled)
    if dsp.binaural.backend:
        binaural["backend"] = dsp.binaural.backend
    direction["follow_steering"] = bool(dsp.binaural.follow_beamformer_steering)
    direction["azimuth_deg"] = float(dsp.binaural.azimuth_deg)
    direction["elevation_deg"] = float(dsp.binaural.elevation_deg)
    if dsp.spatial_backend or dsp.spatial_profile_path:
        spatial = raw.setdefault("spatial", {})
        if not isinstance(spatial, dict):
            raise RuntimeError("Runtime config field 'spatial' must be a mapping.")
        if dsp.spatial_backend:
            spatial["backend"] = str(dsp.spatial_backend)
        if dsp.spatial_profile_path:
            profile = Path(dsp.spatial_profile_path)
            spatial["profile_path"] = profile.name if profile.is_absolute() else str(profile)
        if str(spatial.get("backend")) == "fixed_measured":
            binaural["enabled"] = False
            steering["experimental_dual_reference_mvdr"] = False


def patch_runtime_yaml_dsp(path: str | Path, dsp: DspCommitSnapshot) -> Path:
    """Rewrite session/runtime YAML with the current testbench DSP snapshot."""
    dest = Path(path)
    with open(dest, encoding="utf-8") as handle:
        raw = yaml.safe_load(handle)
    if not isinstance(raw, dict):
        raise RuntimeError(f"Runtime config is not a mapping: {dest}")
    apply_dsp_fields(raw, dsp)
    dest.write_text(yaml.safe_dump(raw, sort_keys=False), encoding="utf-8")
    return dest


def commit_runtime_config(
    dest: str | Path = DEFAULT_CONFIG_PATH,
    *,
    base_config: str | Path = DEFAULT_CONFIG_PATH,
    calibration_src: str | Path,
    common_eq_enabled: bool = False,
    common_eq_sections: list[dict[str, Any]] | None = None,
    dsp: DspCommitSnapshot,
) -> UploadCommitResult:
    """Commit calibration + DSP knob selections into runtime YAML for next start."""
    dest_path = Path(dest)
    base_path = Path(base_config)
    calibration_src_path = Path(calibration_src)
    if not calibration_src_path.is_file():
        raise FileNotFoundError(f"Calibration YAML does not exist: {calibration_src_path}")
    if not base_path.is_file():
        raise FileNotFoundError(f"Base runtime config does not exist: {base_path}")

    with open(base_path, encoding="utf-8") as handle:
        raw = yaml.safe_load(handle)
    if not isinstance(raw, dict):
        raise RuntimeError(f"Runtime config is not a mapping: {base_path}")

    dest_path.parent.mkdir(parents=True, exist_ok=True)
    backup_path = _next_backup_path(dest_path)
    if not dest_path.is_file():
        raise FileNotFoundError(f"Destination runtime config does not exist: {dest_path}")
    copy2(dest_path, backup_path)

    calibration_copy_path = dest_path.parent / _UPLOADED_CALIBRATION_NAME
    copy2(calibration_src_path, calibration_copy_path)
    raw["calibration_path"] = _UPLOADED_CALIBRATION_NAME
    apply_dsp_fields(raw, dsp)

    if common_eq_sections is not None:
        raw["common_eq"] = {
            "enabled": bool(common_eq_enabled),
            "sections": _normalize_common_eq_sections(common_eq_sections),
        }

    dest_path.write_text(yaml.safe_dump(raw, sort_keys=False), encoding="utf-8")
    suppression = raw["suppression"]
    mode = parse_suppression_mode(dsp.suppression_mode)
    return UploadCommitResult(
        committed_config_path=dest_path,
        calibration_copy_path=calibration_copy_path,
        backup_path=backup_path,
        resolved_suppression_enabled=bool(suppression["enabled"]),
        suppression_mode=mode,
        spatial_backend=str(raw.get("spatial", {}).get("backend")) if isinstance(raw.get("spatial"), dict) else None,
        spatial_profile_path=(
            Path(str(raw["spatial"]["profile_path"])) if isinstance(raw.get("spatial"), dict) and raw["spatial"].get("profile_path") else None
        ),
    )
