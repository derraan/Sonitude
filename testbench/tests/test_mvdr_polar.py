"""IR / near-field MVDR polar diagnostics."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest
import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from calibration.geometry import load_geometry, load_mvdr_tuning  # noqa: E402
from calibration.mvdr_polar import (  # noqa: E402
    atf_from_irs,
    beampattern_db,
    main as polar_main,
    mvdr_weights_for_look,
    polar_plot_coords,
    speech_bin_slice,
    summarize_suppression,
    _synthetic_nearfield_irs,
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
    # Ears slightly behind the arc so front/back is not ambiguous.
    assert mics["M0_left_ear"]["y"] < 0.0
    assert mics["M5_right_ear"]["y"] < 0.0
    loaded = load_geometry(geom_path)
    assert loaded.xyz_m[0, 0] < 0.0
    assert loaded.xyz_m[5, 0] > 0.0


def test_polar_plot_puts_look_zero_at_north():
    """Regression: do not double-rotate so look=0° lands on the East lobe."""
    az = np.array([-90.0, 0.0, 90.0, 180.0])
    # Look-normalized: peak at 0°, deep nulls at ±90°.
    pat = np.array([-15.0, 0.0, -15.0, -8.0])
    theta, radius = polar_plot_coords(az, pat)
    # Drop closing sample for argmax.
    peak_i = int(np.argmax(radius[:-1]))
    peak_theta = float(theta[peak_i])
    # With theta_zero='N' and clockwise, mpl theta≈0 is the top (0° label).
    assert abs(peak_theta) < 1e-9
    assert abs(float(az[np.argsort(az)][peak_i])) < 1e-9


def test_nearfield_mvdr_polar_peaks_near_look():
    geometry = ROOT / "config" / "geometry_soundbubble_initial.yaml"
    mvdr = load_mvdr_tuning(ROOT / "config" / "default.yaml")
    sr = 44100
    fft = 128
    azimuths = np.arange(-180.0, 180.0, 15.0)
    irs = _synthetic_nearfield_irs(geometry, sr, azimuths, distance_m=1.0)
    h = atf_from_irs(irs, fft)
    look_az = 0.0
    look_i = int(np.argmin(np.abs(azimuths - look_az)))
    w, _ = mvdr_weights_for_look(
        h,
        look_i,
        reference_mic=2,
        exclude_look=True,
        self_noise=1e-3,
        max_weight_norm=mvdr["max_white_noise_gain"],
        diag_load=mvdr["diag_load"],
    )
    pat = beampattern_db(w, h, look_index=look_i, speech_bins=speech_bin_slice(fft, sr))
    summary = summarize_suppression(azimuths, pat, look_az)
    assert abs(summary["peak_az_deg"]) <= 15.0
    assert summary["suppression_db"] > 6.0


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


def test_polar_cli_synthetic(tmp_path: Path):
    plot = tmp_path / "polar.png"
    report = tmp_path / "report.json"
    try:
        import matplotlib  # noqa: F401
    except ImportError:
        pytest.skip("matplotlib required for polar PNG")
    rc = polar_main(
        [
            "--synthetic",
            "--geometry",
            str(ROOT / "config" / "geometry_soundbubble_initial.yaml"),
            "--look-az",
            "0",
            "--az-step",
            "30",
            "--plot",
            str(plot),
            "--json-out",
            str(report),
        ]
    )
    assert rc == 0
    assert plot.exists()
    assert report.exists()
    assert "suppression_db" in report.read_text(encoding="utf-8")
