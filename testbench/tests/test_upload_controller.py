from __future__ import annotations

from pathlib import Path

import yaml

from app.controller.upload_controller import DspCommitSnapshot, commit_runtime_config
from app.storage.models import BinauralRequest, SuppressorRequest


def _write_runtime_config(path: Path, suppression_enabled: bool) -> None:
    raw = {
        "capture": {"sample_rate_hz": 44100},
        "active_channel_map": [0, 1, 2, 3, 4, 5],
        "geometry_path": "geometry_soundbubble_initial.yaml",
        "calibration_path": "calibration_example.yaml",
        "suppression": {
            "enabled": suppression_enabled,
            "backend": "conservative",
            "fade_ms": 120.0,
            "activity_threshold": 0.03,
            "confidence_threshold": 0.6,
        },
        "steering": {"ambient_floor_linear": 0.25},
        "binaural": {
            "enabled": False,
            "backend": "mono_reference",
            "direction": {"follow_steering": False, "azimuth_deg": 0.0, "elevation_deg": 0.0},
        },
        "odas": {"endpoint": "unix:///tmp/odas.sock"},
        "common_eq": {"enabled": False, "sections": []},
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(yaml.safe_dump(raw, sort_keys=False), encoding="utf-8")


def _snapshot(mode: str) -> DspCommitSnapshot:
    return DspCommitSnapshot(
        suppression_mode=mode,
        suppression_backend="spectral",
        suppressor=SuppressorRequest(
            ambient_floor_linear=0.12,
            fade_ms=88.0,
            activity_threshold=0.045,
            confidence_threshold=0.7,
            envelope_attack_coeff=0.4,
            envelope_release_coeff=0.02,
            confidence=0.55,
            focus_active=True,
        ),
        binaural=BinauralRequest(
            enabled=True,
            backend="compact_hrtf",
            azimuth_deg=35.0,
            elevation_deg=10.0,
            follow_beamformer_steering=True,
        ),
    )


def test_commit_runtime_config_writes_backup_and_patches_fields(tmp_path: Path) -> None:
    config_path = tmp_path / "config" / "default.yaml"
    _write_runtime_config(config_path, suppression_enabled=False)
    cal_src = tmp_path / "compiled" / "calibration_session_E_full.yaml"
    cal_src.parent.mkdir(parents=True, exist_ok=True)
    cal_src.write_text("sample_rate_hz: 44100\n", encoding="utf-8")

    result = commit_runtime_config(
        config_path,
        base_config=config_path,
        calibration_src=cal_src,
        common_eq_enabled=True,
        common_eq_sections=[{"ftype": "PK", "freq_hz": 1200.0, "gain_db": 1.25, "q": 1.1}],
        dsp=_snapshot("on"),
    )

    assert result.committed_config_path == config_path
    assert result.calibration_copy_path == config_path.parent / "calibration_uploaded.yaml"
    assert result.calibration_copy_path.read_text(encoding="utf-8") == cal_src.read_text(encoding="utf-8")
    assert result.backup_path.parent == config_path.parent / "backups"
    assert result.backup_path.is_file()

    raw = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    assert raw["calibration_path"] == "calibration_uploaded.yaml"
    assert raw["geometry_path"] == "geometry_soundbubble_initial.yaml"
    assert raw["suppression"]["enabled"] is True
    assert raw["suppression"]["backend"] == "spectral"
    assert raw["suppression"]["fade_ms"] == 88.0
    assert raw["suppression"]["activity_threshold"] == 0.045
    assert raw["suppression"]["confidence_threshold"] == 0.7
    assert raw["steering"]["ambient_floor_linear"] == 0.12
    assert raw["binaural"]["enabled"] is True
    assert raw["binaural"]["backend"] == "compact_hrtf"
    assert raw["binaural"]["direction"]["follow_steering"] is True
    assert raw["binaural"]["direction"]["azimuth_deg"] == 35.0
    assert raw["binaural"]["direction"]["elevation_deg"] == 10.0
    assert raw["common_eq"]["enabled"] is True
    assert raw["common_eq"]["sections"][0]["type"] == "PK"
    assert raw["odas"]["endpoint"] == "unix:///tmp/odas.sock"


def test_commit_runtime_config_auto_mode_preserves_yaml_enabled(tmp_path: Path) -> None:
    config_path = tmp_path / "config" / "default.yaml"
    _write_runtime_config(config_path, suppression_enabled=True)
    cal_src = tmp_path / "compiled" / "calibration_session_A_baseline.yaml"
    cal_src.parent.mkdir(parents=True, exist_ok=True)
    cal_src.write_text("sample_rate_hz: 44100\n", encoding="utf-8")

    commit_runtime_config(
        config_path,
        base_config=config_path,
        calibration_src=cal_src,
        dsp=_snapshot("auto"),
    )

    raw = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    assert raw["suppression"]["enabled"] is True
