"""Offline calibration compiler controller.

Runs `tools.calibration.compile_session` on a QThread so the test-bench GUI
stays responsive. DSP still lives in C++; this only produces YAML artifacts
and an optional runtime-config overlay the Recorded / Real-Time tabs can load.
"""

from __future__ import annotations

import sys
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml
from PySide6.QtCore import QThread, Signal

from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.processing.sonitude_binary_locator import find_repo_root
from app.storage.result_store import DEFAULT_DATA_ROOT

_repo_root = str(find_repo_root())
if _repo_root not in sys.path:
    sys.path.insert(0, _repo_root)

from tools.calibration.compile_calibration import (  # noqa: E402
    CalibrationCompileRequest,
    compile_session,
)
from tools.calibration.compile_array import main as compile_array_main  # noqa: E402
from tools.calibration.geometry import MIC_IDS  # noqa: E402
from tools.calibration.angles import STANDARD_ARRAY_AZIMUTHS_DEG, format_azimuth_label  # noqa: E402
from tools.calibration.mdat_parse import (  # noqa: E402
    MdatParseResult,
    merge_mdat_results,
    parse_rew_mdat,
    summarize_mdat_markdown,
)

__all__ = [
    "DEFAULT_CALIBRATION_OUT_DIR",
    "MIC_IDS",
    "STANDARD_ARRAY_AZIMUTHS_DEG",
    "VARIANT_NOTES",
    "VARIANT_ORDER",
    "CalibrationCompileRequest",
    "CalibrationWorker",
    "compile_session",
    "default_geometry_path",
    "format_azimuth_label",
    "merge_mdat_results",
    "missing_compile_inputs",
    "MeasuredProfileCompileRequest",
    "MeasuredProfileWorker",
    "compile_measured_profile",
    "parse_rew_mdat",
    "resolve_config_sidecar",
    "summarize_mdat_markdown",
    "write_dsp_runtime_overlay",
]

DEFAULT_CALIBRATION_OUT_DIR = DEFAULT_DATA_ROOT / "calibration"
VARIANT_ORDER = ("A_baseline", "B_delay", "C_polarity", "D_delay_polarity", "E_full")
VARIANT_NOTES = {
    "A_baseline": "Identity polarity/gain/delay (control).",
    "B_delay": "Measured delay only.",
    "C_polarity": "M5 invert test; delay and gain identity.",
    "D_delay_polarity": "Measured delay plus M5 invert test.",
    "E_full": "Measured delay, gain, and M5 invert test.",
}


@dataclass(frozen=True)
class MeasuredProfileCompileRequest:
    out_prefix: Path
    sample_rate_hz: int
    geometry_id: str
    reference_mic: int
    left_ear_mic: int
    right_ear_mic: int
    calibration_yaml: Path
    stimulus_wav: Path
    directions: tuple[tuple[float, Path], ...]
    window_pre_samples: int = 8
    window_length_samples: int = 256
    window_taper: str = "tukey"
    window_tukey_alpha: float = 0.25
    max_weight_norm: float = 4.0
    self_noise: float = 1.0e-3


def compile_measured_profile(request: MeasuredProfileCompileRequest) -> dict[str, Any]:
    request.out_prefix.parent.mkdir(parents=True, exist_ok=True)
    manifest_path = request.out_prefix.with_suffix(".manifest.json")
    manifest: dict[str, Any] = {
        "schema_version": 2,
        "input": "sweep",
        "stimulus": str(request.stimulus_wav.resolve()),
        "sample_rate_hz": int(request.sample_rate_hz),
        "geometry_id": str(request.geometry_id),
        "layout": "multichannel",
        "fft_size": 128,
        "hop_size": 32,
        "reference_mic": int(request.reference_mic),
        "left_ear_mic": int(request.left_ear_mic),
        "right_ear_mic": int(request.right_ear_mic),
        "self_noise": float(request.self_noise),
        "max_weight_norm": float(request.max_weight_norm),
        "calibration_yaml": str(request.calibration_yaml.resolve()),
        "window": {
            "pre_samples": int(request.window_pre_samples),
            "length_samples": int(request.window_length_samples),
            "taper": str(request.window_taper),
            "tukey_alpha": float(request.window_tukey_alpha),
        },
        "directions": [
            {"azimuth_deg": float(az), "path": str(path.resolve())}
            for az, path in request.directions
        ],
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    code = compile_array_main(
        ["--manifest", str(manifest_path), "--output-prefix", str(request.out_prefix)]
    )
    if code != 0:
        raise RuntimeError("compile_array failed")
    report_path = request.out_prefix.with_suffix(".report.json")
    if not report_path.is_file():
        raise RuntimeError(f"Missing measured profile report: {report_path}")
    return json.loads(report_path.read_text(encoding="utf-8"))


def default_geometry_path(config_path: str | Path = DEFAULT_CONFIG_PATH) -> Path:
    summary = read_runtime_config_summary(config_path)
    return resolve_config_sidecar(Path(config_path), summary.geometry_path)


def resolve_config_sidecar(config_path: Path, relative_or_abs: str) -> Path:
    candidate = Path(relative_or_abs)
    if candidate.is_absolute():
        return candidate
    return (Path(config_path).parent / candidate).resolve()


def _absolutize_runtime_paths(raw: dict[str, Any], base_config: Path) -> None:
    def abs_field(container: dict[str, Any], key: str) -> None:
        value = container.get(key)
        if not value:
            return
        path = Path(str(value))
        if not path.is_absolute():
            container[key] = str((base_config.parent / path).resolve())

    abs_field(raw, "geometry_path")
    abs_field(raw, "calibration_path")
    binaural = raw.get("binaural")
    if isinstance(binaural, dict):
        profile = binaural.get("profile")
        if isinstance(profile, dict):
            abs_field(profile, "table_path")
    spatial = raw.get("spatial")
    if isinstance(spatial, dict):
        abs_field(spatial, "profile_path")


def write_dsp_runtime_overlay(
    dest: str | Path,
    *,
    calibration_yaml: str | Path | None = None,
    base_config: str | Path = DEFAULT_CONFIG_PATH,
    common_eq_enabled: bool = False,
    common_eq_sections: list[dict[str, Any]] | None = None,
    spatial_backend: str | None = None,
    spatial_profile_path: str | Path | None = None,
) -> Path:
    """Copy runtime YAML, point it at a compiled calibration, keep other paths valid."""
    base = Path(base_config)
    dest_path = Path(dest)
    with open(base, encoding="utf-8") as handle:
        raw = yaml.safe_load(handle)
    if not isinstance(raw, dict):
        raise RuntimeError(f"Runtime config is not a mapping: {base}")
    _absolutize_runtime_paths(raw, base)
    if calibration_yaml is not None:
        raw["calibration_path"] = str(Path(calibration_yaml).resolve())
    steering = raw.setdefault("steering", {})
    if not isinstance(steering, dict):
        raise RuntimeError("Runtime config field 'steering' must be a mapping.")
    steering["model"] = "near_field"
    if steering.get("source_distance_m") in (None, ""):
        steering["source_distance_m"] = 0.45
    if spatial_backend or spatial_profile_path:
        spatial = raw.setdefault("spatial", {})
        if not isinstance(spatial, dict):
            raise RuntimeError("Runtime config field 'spatial' must be a mapping.")
        if spatial_backend:
            spatial["backend"] = str(spatial_backend)
        if spatial_profile_path:
            spatial["profile_path"] = str(Path(spatial_profile_path).resolve())
        if str(spatial.get("backend")) == "fixed_measured":
            binaural = raw.setdefault("binaural", {})
            if isinstance(binaural, dict):
                binaural["enabled"] = False
            steering = raw.setdefault("steering", {})
            if isinstance(steering, dict):
                steering["experimental_dual_reference_mvdr"] = False
    if common_eq_sections is not None:
        sections = []
        for section in common_eq_sections:
            sections.append(
                {
                    "type": str(section.get("type", section.get("ftype", "PK"))),
                    "freq_hz": float(section["freq_hz"]),
                    "gain_db": float(section.get("gain_db", 0.0)),
                    "q": float(section["q"]),
                }
            )
        raw["common_eq"] = {"enabled": bool(common_eq_enabled), "sections": sections}
    dest_path.parent.mkdir(parents=True, exist_ok=True)
    dest_path.write_text(yaml.safe_dump(raw, sort_keys=False), encoding="utf-8")
    return dest_path


def missing_compile_inputs(request: CalibrationCompileRequest) -> list[str]:
    missing: list[str] = []
    if not request.array_wav.is_file():
        missing.append("6-channel array WAV")
    for idx, path in enumerate(request.element_wavs):
        if not path.is_file():
            missing.append(f"element WAV {MIC_IDS[idx]}")
    if not request.geometry.is_file():
        missing.append("geometry YAML")
    if request.rew_filter_txt and not Path(request.rew_filter_txt).is_file():
        missing.append("REW filter text")
    if request.delay_mode == "absolute_tof":
        if not request.stimulus_wav or not Path(request.stimulus_wav).is_file():
            missing.append("stimulus WAV (absolute TOF)")
    return missing


class CalibrationWorker(QThread):
    finished_ok = Signal(object)
    failed = Signal(str)

    def __init__(self, request: CalibrationCompileRequest, parent=None) -> None:
        super().__init__(parent)
        self._request = request

    def run(self) -> None:
        try:
            report = compile_session(self._request)
        except Exception as exc:  # noqa: BLE001 - surface compiler errors in the GUI
            self.failed.emit(str(exc))
            return
        self.finished_ok.emit(report)


class MeasuredProfileWorker(QThread):
    finished_ok = Signal(object)
    failed = Signal(str)

    def __init__(self, request: MeasuredProfileCompileRequest, parent=None) -> None:
        super().__init__(parent)
        self._request = request

    def run(self) -> None:
        try:
            report = compile_measured_profile(self._request)
        except Exception as exc:  # noqa: BLE001
            self.failed.emit(str(exc))
            return
        self.finished_ok.emit(report)
