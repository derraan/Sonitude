from __future__ import annotations

from pathlib import Path

from app.controller.runtime_yaml_catalog import (
    list_calibration_yaml_files,
    list_runtime_yaml_files,
)


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def test_catalog_separates_runtime_and_calibration(tmp_path: Path) -> None:
    config = tmp_path / "config"
    data = tmp_path / "data"
    session = tmp_path / "data" / "calibration" / "cal_x"
    _write(
        config / "default.yaml",
        "capture:\n  sample_rate_hz: 44100\ncalibration_path: calibration_example.yaml\n",
    )
    _write(
        config / "calibration_example.yaml",
        "sample_rate_hz: 44100\nchannels:\n- id: M0\n  polarity: 1\n  gain_linear: 1.0\n",
    )
    _write(config / "geometry_soundbubble_initial.yaml", "profile_name: x\nmicrophones: []\n")
    _write(
        session / "runtime_config_overlay.yaml",
        "capture:\n  sample_rate_hz: 44100\ncalibration_path: C:/tmp/cal.yaml\n",
    )
    _write(
        session / "calibration_session_E_full.yaml",
        "sample_rate_hz: 44100\nchannels:\n- polarity: 1\n  gain_linear: 1.0\n",
    )

    runtime = {p.name for p in list_runtime_yaml_files(config_dir=config, data_root=data)}
    calibration = {p.name for p in list_calibration_yaml_files(config_dir=config, data_root=data)}
    assert "default.yaml" in runtime
    assert "runtime_config_overlay.yaml" in runtime
    assert "calibration_example.yaml" in calibration
    assert "calibration_session_E_full.yaml" in calibration
    assert "geometry_soundbubble_initial.yaml" not in runtime
    assert "geometry_soundbubble_initial.yaml" not in calibration
    assert "default.yaml" not in calibration
    assert "calibration_example.yaml" not in runtime
