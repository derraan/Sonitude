"""Offline calibration compiler controller.

Runs `tools.calibration.compile_session` on a QThread so the test-bench GUI
stays responsive. DSP still lives in C++; this only produces YAML artifacts
and an optional runtime-config overlay the Recorded / Real-Time tabs can load.
"""

from __future__ import annotations

import sys
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
from tools.calibration.geometry import MIC_IDS  # noqa: E402
from tools.calibration.angles import STANDARD_ARRAY_AZIMUTHS_DEG, format_azimuth_label  # noqa: E402
from tools.calibration.mdat_parse import MdatParseResult, parse_rew_mdat, summarize_mdat_markdown  # noqa: E402

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
    "missing_compile_inputs",
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


def write_dsp_runtime_overlay(
    dest: str | Path,
    *,
    calibration_yaml: str | Path,
    base_config: str | Path = DEFAULT_CONFIG_PATH,
    common_eq_enabled: bool = False,
    common_eq_sections: list[dict[str, Any]] | None = None,
) -> Path:
    """Copy runtime YAML, point it at a compiled calibration, keep other paths valid."""
    base = Path(base_config)
    dest_path = Path(dest)
    with open(base, encoding="utf-8") as handle:
        raw = yaml.safe_load(handle)
    if not isinstance(raw, dict):
        raise RuntimeError(f"Runtime config is not a mapping: {base}")
    _absolutize_runtime_paths(raw, base)
    raw["calibration_path"] = str(Path(calibration_yaml).resolve())
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
