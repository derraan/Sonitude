"""Host-DSP MVDR polar diagnostics."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest
import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from calibration.compile_array import compile_from_irs  # noqa: E402
from calibration.geometry import load_geometry  # noqa: E402
from calibration.mvdr_polar import (  # noqa: E402
    MissingBinaryError,
    find_wav_replay,
    load_probe_recordings,
    main as polar_main,
    polar_plot_coords,
    summarize_suppression,
)


def test_geometry_head_frame_places_ears_on_x_axis():
    geom_path = ROOT / "config" / "geometry_soundbubble_initial.yaml"
    geom = yaml.safe_load(geom_path.read_text())
    mics = {m["id"]: m for m in geom["microphones"]}
    assert geom["frame"]["convention"] == "head_frame_v1"
    assert geom["frame"]["right"] == "+X"
    assert geom["frame"]["forward"] == "+Y"
    assert geom["frame"]["up"] == "+Z"
    assert mics["M0_left_ear"]["x"] < 0.0
    assert mics["M5_right_ear"]["x"] > 0.0
    assert mics["M0_left_ear"]["y"] < 0.0
    assert mics["M5_right_ear"]["y"] < 0.0
    loaded = load_geometry(geom_path)
    assert loaded.xyz_m[0, 0] < 0.0
    assert loaded.xyz_m[5, 0] > 0.0


def test_polar_plot_puts_look_zero_at_north():
    """Regression: do not double-rotate so look=0° lands on the East lobe."""
    az = np.array([-90.0, 0.0, 90.0, 180.0])
    pat = np.array([-15.0, 0.0, -15.0, -8.0])
    theta, radius = polar_plot_coords(az, pat)
    peak_i = int(np.argmax(radius[:-1]))
    peak_theta = float(theta[peak_i])
    assert abs(peak_theta) < 1e-9
    assert abs(float(az[np.argsort(az)][peak_i])) < 1e-9


def test_polar_plot_does_not_close_front_hemisphere():
    az = np.array([-90.0, -60.0, 0.0, 60.0, 90.0])
    pat = np.zeros(5)
    theta, radius = polar_plot_coords(az, pat)
    assert theta.size == az.size
    assert radius.size == az.size


def test_axis_swap_rejected_by_geometry_loader():
    geometry = ROOT / "config" / "geometry_soundbubble_initial.yaml"
    broken = yaml.safe_load(geometry.read_text())
    for mic in broken["microphones"]:
        mic["x"], mic["y"] = mic["y"], mic["x"]
    broken_path = Path("/tmp/geom_swapped_axes.yaml")
    broken_path.write_text(yaml.safe_dump(broken), encoding="utf-8")
    with pytest.raises(RuntimeError, match="USB0"):
        load_geometry(broken_path)


def test_wrong_frame_labels_rejected():
    geometry = ROOT / "config" / "geometry_soundbubble_initial.yaml"
    broken = yaml.safe_load(geometry.read_text())
    broken["frame"]["forward"] = "+X"
    broken["frame"]["right"] = "+Y"
    broken_path = Path("/tmp/geom_wrong_frame.yaml")
    broken_path.write_text(yaml.safe_dump(broken), encoding="utf-8")
    with pytest.raises(RuntimeError, match="head_frame"):
        load_geometry(broken_path)


def test_compile_from_irs_exclude_look_default_preserves_distortionless():
    irs = np.zeros((3, 6, 256), dtype=np.float64)
    for di, base in enumerate([0.0, 2.0, 4.0]):
        for m in range(6):
            t0 = 64 + base + 0.4 * m
            t = np.arange(256)
            irs[di, m] = np.exp(-0.5 * ((t - t0) / 2.0) ** 2)
    profile = compile_from_irs(irs, [0.0, 60.0, -60.0], 44100, reference_mic=2)
    w = profile["weights"]
    d = profile["steering"]
    for di in range(3):
        for k in range(profile["bin_count"]):
            if not profile["valid"][di, k]:
                continue
            unity = np.vdot(d[di, k], w[di, k])
            assert abs(unity - 1.0) < 1e-2


def test_load_probe_recordings_requires_existing_wavs(tmp_path: Path):
    manifest = tmp_path / "manifest.yaml"
    manifest.write_text(
        "layout: multichannel\ndirections:\n  - azimuth_deg: 0\n    path: missing.wav\n",
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="missing capture"):
        load_probe_recordings(manifest)


def test_polar_cli_synthetic_host_dsp(tmp_path: Path):
    try:
        import matplotlib  # noqa: F401
    except ImportError:
        pytest.skip("matplotlib required for polar PNG")
    try:
        find_wav_replay()
    except MissingBinaryError as exc:
        pytest.skip(str(exc))
    plot = tmp_path / "polar.png"
    report = tmp_path / "report.json"
    rc = polar_main(
        [
            "--synthetic",
            "--runtime",
            str(ROOT / "config" / "default.yaml"),
            "--look-az",
            "0",
            "--az-step",
            "90",
            "--duration-s",
            "0.6",
            "--plot",
            str(plot),
            "--json-out",
            str(report),
            "--work-dir",
            str(tmp_path / "work"),
        ]
    )
    assert rc == 0
    assert plot.exists()
    payload = json.loads(report.read_text(encoding="utf-8"))
    assert payload["processor"] == "sonitude_wav_replay"
    assert payload["beamformer"] == "dsp::MvdrBeamformer"
    summary = payload["primary"]["summary"]
    assert abs(summary["peak_az_deg"]) <= 90.0
    assert "suppression_db" in summary


def test_summarize_suppression_look_normalized():
    az = np.array([-90.0, 0.0, 90.0])
    pat = np.array([-8.0, 0.0, -8.0])
    summary = summarize_suppression(az, pat, 0.0)
    assert summary["peak_az_deg"] == 0.0
    assert summary["suppression_db"] == pytest.approx(8.0)
