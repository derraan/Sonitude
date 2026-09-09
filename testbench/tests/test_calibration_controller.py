from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf
import yaml

from app.config_reader import DEFAULT_CONFIG_PATH
from app.controller.calibration_controller import (
    STANDARD_ARRAY_AZIMUTHS_DEG,
    CalibrationCompileRequest,
    compile_session,
    default_geometry_path,
    format_azimuth_label,
    missing_compile_inputs,
    parse_rew_mdat,
    write_dsp_runtime_overlay,
)


def _write_synth_set(tmp_path: Path, *, with_stimulus: bool = False):
    fs = 44100
    n = 8192
    rng = np.random.default_rng(7)
    base = rng.normal(0.0, 0.25, size=n).astype(np.float64)
    # Mild band-limit so bandpass stages still have energy.
    from scipy.signal import butter, filtfilt

    b, a = butter(2, [300.0 / (0.5 * fs), 8000.0 / (0.5 * fs)], btype="band")
    base = filtfilt(b, a, base).astype(np.float64)
    tof = np.array([40.0, 42.0, 40.0, 39.0, 41.5, 38.0], dtype=np.float64)
    # Build a longer capture so absolute TOF has room after the stimulus.
    capture_n = n + 128
    array = np.zeros((capture_n, 6), dtype=np.float64)
    for idx, delay in enumerate(tof):
        d = int(delay)
        array[d : d + n, idx] = base
    array0 = tmp_path / "array0.wav"
    sf.write(str(array0), array.astype(np.float32), fs, subtype="FLOAT")
    element_paths = []
    for idx in range(6):
        path = tmp_path / f"m{idx}.wav"
        sf.write(str(path), base.astype(np.float32), fs, subtype="FLOAT")
        element_paths.append(path)
    stim_path = None
    if with_stimulus:
        stim_path = tmp_path / "stimulus.wav"
        sf.write(str(stim_path), base.astype(np.float32), fs, subtype="FLOAT")
    return fs, array0, element_paths, stim_path, tof


def test_standard_azimuth_grid_includes_requested_angles() -> None:
    assert 0.0 in STANDARD_ARRAY_AZIMUTHS_DEG
    assert 30.0 in STANDARD_ARRAY_AZIMUTHS_DEG
    assert -180.0 in STANDARD_ARRAY_AZIMUTHS_DEG
    assert 140.0 in STANDARD_ARRAY_AZIMUTHS_DEG
    assert format_azimuth_label(30.0) == "+30°"
    assert format_azimuth_label(-90.0) == "-90°"
    assert format_azimuth_label(0.0) == "0°"


def test_parse_rew_mdat_extracts_delay_and_levels() -> None:
    mdat = Path(
        r"C:\Users\darre\3301PrototypeS2\Sonitude-spectral-postfilter"
        r"\tmp_cal_unzip\Characterization 8-9-26 (2)\export"
        r"\Sonitude-Characterization-ProtoV1-(2).mdat"
    )
    if not mdat.is_file():
        import pytest

        pytest.skip("characterization MDAT fixture not present")
    result = parse_rew_mdat(mdat)
    assert result.sample_rate_hz_guess == 44100.0
    assert len(result.measurements) >= 12
    array_delays = result.array_channel_delays_ms(0.0)
    assert set(array_delays) == {1, 2, 3, 4, 5, 6}
    assert abs(array_delays[3] - 2.7789) < 1e-3
    elements = result.element_delays_ms()
    assert "M0" in elements
    assert any(m.timing_peak_dbfs is not None for m in result.measurements)


def test_missing_compile_inputs_lists_absent_files(tmp_path: Path) -> None:
    request = CalibrationCompileRequest(
        array_wav=tmp_path / "missing_array.wav",
        element_wavs=tuple(tmp_path / f"m{i}.wav" for i in range(6)),  # type: ignore[arg-type]
        geometry=tmp_path / "missing_geom.yaml",
        out_dir=tmp_path / "out",
        rew_filter_txt=str(tmp_path / "missing_rew.txt"),
        delay_mode="absolute_tof",
    )
    missing = missing_compile_inputs(request)
    assert "6-channel array WAV" in missing
    assert "geometry YAML" in missing
    assert "REW filter text" in missing
    assert "stimulus WAV (absolute TOF)" in missing


def test_write_dsp_runtime_overlay_absolutizes_paths(tmp_path: Path) -> None:
    cal_yaml = tmp_path / "calibration_session_E_full.yaml"
    cal_yaml.write_text("sample_rate_hz: 44100\n", encoding="utf-8")
    dest = tmp_path / "nested" / "runtime_config_overlay.yaml"
    sections = [{"ftype": "PK", "freq_hz": 1000.0, "gain_db": 1.5, "q": 1.2}]
    written = write_dsp_runtime_overlay(
        dest,
        calibration_yaml=cal_yaml,
        base_config=DEFAULT_CONFIG_PATH,
        common_eq_enabled=True,
        common_eq_sections=sections,
    )
    raw = yaml.safe_load(written.read_text(encoding="utf-8"))
    assert Path(raw["calibration_path"]) == cal_yaml.resolve()
    assert Path(raw["geometry_path"]).is_file()
    assert raw["common_eq"]["sections"][0]["type"] == "PK"


def test_compile_session_relative_with_extra_angle(tmp_path: Path) -> None:
    fs, array0, element_paths, _stim, _tof = _write_synth_set(tmp_path)
    array30 = tmp_path / "array30.wav"
    pcm, _ = sf.read(str(array0), always_2d=True)
    sf.write(str(array30), pcm.astype(np.float32), fs, subtype="FLOAT")
    report = compile_session(
        CalibrationCompileRequest(
            array_wav=array0,
            element_wavs=tuple(element_paths),  # type: ignore[arg-type]
            geometry=default_geometry_path(),
            out_dir=tmp_path / "out",
            tag="gui",
            azimuth_deg=0.0,
            max_lag_samples=64,
            extra_array_angles=((30.0, array30),),
            delay_mode="relative",
        )
    )
    assert report["delay_mode"] == "relative"
    assert report["primary_azimuth_deg"] == 0.0
    assert len(report["angles"]) == 2
    assert report["angles"][1]["azimuth_deg"] == 30.0
    assert any("multi_angle" in w for w in report["warnings"])


def test_compile_session_absolute_tof(tmp_path: Path) -> None:
    fs, array0, element_paths, stim_path, tof = _write_synth_set(tmp_path, with_stimulus=True)
    assert stim_path is not None
    report = compile_session(
        CalibrationCompileRequest(
            array_wav=array0,
            element_wavs=tuple(element_paths),  # type: ignore[arg-type]
            geometry=default_geometry_path(),
            out_dir=tmp_path / "out_tof",
            tag="tof",
            azimuth_deg=0.0,
            delay_mode="absolute_tof",
            stimulus_wav=str(stim_path),
            max_tof_samples=128,
        )
    )
    assert report["delay_mode"] == "absolute_tof"
    abs_rows = report["delay"]["absolute_tof"]
    assert abs_rows is not None
    measured = np.array([row["tof_samples"] for row in abs_rows], dtype=np.float64)
    # Absolute TOF recovers stimulus->mic lag within ~1 sample.
    assert np.max(np.abs(measured - tof)) < 1.25
    # Relative lag vs reference index 2 should match TOF differences.
    rel = measured - measured[2]
    report_rel = np.array([row["lag_samples"] for row in report["delay"]["by_mic"]], dtype=np.float64)
    assert np.max(np.abs(report_rel - rel)) < 1e-6
