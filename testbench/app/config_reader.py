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
class BinauralConfigSummary:
    enabled: bool = False
    backend: str = "mono_reference"
    follow_steering: bool = True
    azimuth_deg: float = 0.0
    elevation_deg: float = 0.0
    table_path: str = ""


@dataclass
class SuppressionConfigSummary:
    enabled: bool = False
    backend: str = "spectral"
    fade_ms: float = 120.0
    activity_threshold: float = 0.03
    confidence_threshold: float = 0.6
    ambient_floor_linear: float = 0.25
    envelope_attack_coeff: float = 0.35
    envelope_release_coeff: float = 0.01


@dataclass
class RuntimeConfigSummary:
    path: Path
    capture_sample_rate_hz: int
    active_channel_map: list[int]
    geometry_path: str
    calibration_path: str
    suppression_enabled: bool
    suppression: SuppressionConfigSummary
    binaural: BinauralConfigSummary


def _read_binaural_summary(raw: dict) -> BinauralConfigSummary:
    section = raw.get("binaural") or {}
    direction = section.get("direction") or {}
    profile = section.get("profile") or {}
    return BinauralConfigSummary(
        enabled=bool(section.get("enabled", False)),
        backend=str(section.get("backend", "mono_reference")),
        follow_steering=bool(direction.get("follow_steering", True)),
        azimuth_deg=float(direction.get("azimuth_deg", 0.0)),
        elevation_deg=float(direction.get("elevation_deg", 0.0)),
        table_path=str(profile.get("table_path", "")),
    )


def _read_suppression_summary(raw: dict) -> SuppressionConfigSummary:
    section = raw.get("suppression") or {}
    steering = raw.get("steering") or {}
    return SuppressionConfigSummary(
        enabled=bool(section.get("enabled", False)),
        backend=str(section.get("backend", "spectral")),
        fade_ms=float(section.get("fade_ms", 120.0)),
        activity_threshold=float(section.get("activity_threshold", 0.03)),
        confidence_threshold=float(section.get("confidence_threshold", 0.6)),
        ambient_floor_linear=float(steering.get("ambient_floor_linear", 0.25)),
        envelope_attack_coeff=float(section.get("envelope_attack_coeff", 0.35)),
        envelope_release_coeff=float(section.get("envelope_release_coeff", 0.01)),
    )


def read_runtime_config_summary(path: str | Path = DEFAULT_CONFIG_PATH) -> RuntimeConfigSummary:
    path = Path(path)
    with open(path, encoding="utf-8") as handle:
        raw = yaml.safe_load(handle)
    suppression = _read_suppression_summary(raw)
    return RuntimeConfigSummary(
        path=path,
        capture_sample_rate_hz=int(raw["capture"]["sample_rate_hz"]),
        active_channel_map=list(raw.get("active_channel_map", [0, 1, 2, 3, 4, 5])),
        geometry_path=str(raw.get("geometry_path", "")),
        calibration_path=str(raw.get("calibration_path", "")),
        suppression_enabled=suppression.enabled,
        suppression=suppression,
        binaural=_read_binaural_summary(raw),
    )


def binaural_request_from_config(path: str | Path = DEFAULT_CONFIG_PATH):
    """Build a BinauralRequest from runtime YAML (mirrors C++ binaural defaults)."""
    from app.storage.models import BinauralRequest

    summary = read_runtime_config_summary(path)
    b = summary.binaural
    return BinauralRequest(
        enabled=b.enabled,
        backend=b.backend,
        azimuth_deg=b.azimuth_deg,
        elevation_deg=b.elevation_deg,
        follow_beamformer_steering=b.follow_steering,
    )


def suppressor_request_from_config(path: str | Path = DEFAULT_CONFIG_PATH):
    """Build a SuppressorRequest from runtime YAML (mirrors C++ suppressor defaults)."""
    from app.storage.models import SuppressorRequest

    s = read_runtime_config_summary(path).suppression
    return SuppressorRequest(
        ambient_floor_linear=s.ambient_floor_linear,
        fade_ms=s.fade_ms,
        activity_threshold=s.activity_threshold,
        confidence_threshold=s.confidence_threshold,
        envelope_attack_coeff=s.envelope_attack_coeff,
        envelope_release_coeff=s.envelope_release_coeff,
        confidence=1.0,
        focus_active=True,
    )
