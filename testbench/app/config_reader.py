"""Minimal reader for the Sonitude runtime YAML config.

This does NOT replace or duplicate the authoritative C++ config loader/
validator (src/app/config.cpp) — that still runs, and rejects bad configs,
every time a CLI tool is invoked. This module only reads the handful of
fields the GUI needs for display and pre-flight validation (expected sample
rate, active channel map) so the user gets fast feedback before spending time
on a subprocess call.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import yaml

DEFAULT_CONFIG_PATH = Path(__file__).resolve().parents[2] / "config" / "default.yaml"


@dataclass
class RuntimeConfigSummary:
    path: Path
    capture_sample_rate_hz: int
    active_channel_map: list[int]
    geometry_path: str
    calibration_path: str
    suppression_enabled: bool


def read_runtime_config_summary(path: str | Path = DEFAULT_CONFIG_PATH) -> RuntimeConfigSummary:
    path = Path(path)
    with open(path, encoding="utf-8") as handle:
        raw = yaml.safe_load(handle)
    return RuntimeConfigSummary(
        path=path,
        capture_sample_rate_hz=int(raw["capture"]["sample_rate_hz"]),
        active_channel_map=list(raw.get("active_channel_map", [0, 1, 2, 3, 4, 5])),
        geometry_path=str(raw.get("geometry_path", "")),
        calibration_path=str(raw.get("calibration_path", "")),
        suppression_enabled=bool(raw.get("suppression", {}).get("enabled", False)),
    )
