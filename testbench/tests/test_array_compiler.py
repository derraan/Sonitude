import json
import sys
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from calibration.compile_array import (
    compile_from_irs,
    load_ir_cube,
    load_manifest,
    main,
    make_synthetic_impulse_cube,
    pack_profile,
)


def _write_multichannel(path: Path, cube: np.ndarray, direction_index: int, sr: int) -> None:
    sf.write(str(path), cube[direction_index].T, sr, subtype="FLOAT")


def _write_per_mic(directory: Path, cube: np.ndarray, direction_index: int, sr: int) -> list[str]:
    directory.mkdir(parents=True, exist_ok=True)
    out: list[str] = []
    for mic in range(cube.shape[1]):
        p = directory / f"m{mic}.wav"
        sf.write(str(p), cube[direction_index, mic], sr, subtype="FLOAT")
        out.append(str(p))
    return out


def test_synthetic_compiler_distortionless_and_hash():
    irs = make_synthetic_impulse_cube(44100, [[0.0, 0.5, 1.0, 1.5, 2.0, 2.5]])
    profile = compile_from_irs(irs, [0.0], 44100)
    w = profile["weights"]
    d = profile["steering"]
    valid = profile["valid"]
    for k in range(profile["bin_count"]):
        if not valid[0, k]:
            continue
        unity = np.vdot(d[0, k], w[0, k])
        assert abs(unity - 1.0) < 1e-3
    blob = pack_profile(profile)
    assert blob[:4] == b"SMV3"
    assert len(blob) > 128


def test_load_ir_cube_multichannel(tmp_path: Path):
    sample_rate = 44100
    cube = make_synthetic_impulse_cube(sample_rate, [[0.0, 0.5, 1.0, 1.5, 2.0, 2.5], [0.5, 0.0, 0.4, 0.9, 1.3, 1.8]])
    _write_multichannel(tmp_path / "d0.wav", cube, 0, sample_rate)
    _write_multichannel(tmp_path / "d1.wav", cube, 1, sample_rate)
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "sample_rate_hz": sample_rate,
                "layout": "multichannel",
                "directions": [
                    {"azimuth_deg": 0.0, "path": "d0.wav"},
                    {"azimuth_deg": 30.0, "path": "d1.wav"},
                ],
            }
        ),
        encoding="utf-8",
    )
    manifest = load_manifest(manifest_path)
    loaded, azimuths, sr = load_ir_cube(manifest, tmp_path)
    assert sr == sample_rate
    assert azimuths == [0.0, 30.0]
    assert loaded.shape == cube.shape
    assert np.allclose(loaded, cube, atol=1.0e-6)


def test_load_ir_cube_per_mic_matches_multichannel(tmp_path: Path):
    sample_rate = 44100
    cube = make_synthetic_impulse_cube(sample_rate, [[0.0, 0.25, 0.5, 0.75, 1.0, 1.25]])
    _write_multichannel(tmp_path / "single_dir.wav", cube, 0, sample_rate)
    channels = _write_per_mic(tmp_path / "per_mic", cube, 0, sample_rate)
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "sample_rate_hz": sample_rate,
                "layout": "multichannel",
                "directions": [
                    {"azimuth_deg": 0.0, "path": "single_dir.wav"},
                    {"azimuth_deg": -20.0, "layout": "per_mic", "channels": channels},
                ],
            }
        ),
        encoding="utf-8",
    )
    manifest = load_manifest(manifest_path)
    loaded, azimuths, _ = load_ir_cube(manifest, tmp_path)
    assert azimuths == [0.0, -20.0]
    assert np.allclose(loaded[0], loaded[1], atol=1.0e-6)


def test_physical_compile_matches_direct_compile_and_header_flag(tmp_path: Path):
    sample_rate = 44100
    cube = make_synthetic_impulse_cube(sample_rate, [[0.0, 0.5, 1.0, 1.5, 2.0, 2.5], [0.5, 0.0, 0.4, 0.9, 1.3, 1.8]])
    _write_multichannel(tmp_path / "d0.wav", cube, 0, sample_rate)
    _write_multichannel(tmp_path / "d1.wav", cube, 1, sample_rate)
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "sample_rate_hz": sample_rate,
                "layout": "multichannel",
                "geometry_id": "soundbubble_vertical_v1",
                "reference_mic": 2,
                "left_ear_mic": 0,
                "right_ear_mic": 5,
                "directions": [
                    {"azimuth_deg": 0.0, "path": "d0.wav"},
                    {"azimuth_deg": 10.0, "path": "d1.wav"},
                ],
            }
        ),
        encoding="utf-8",
    )

    assert main(["--manifest", str(manifest_path), "--output-prefix", str(tmp_path / "compiled")]) == 0

    manifest = load_manifest(manifest_path)
    loaded, azimuths, _ = load_ir_cube(manifest, tmp_path)
    expected = compile_from_irs(loaded, azimuths, sample_rate, reference_mic=2, left_ear=0, right_ear=5)
    expected["synthetic"] = False
    expected["geometry_id"] = "soundbubble_vertical_v1"
    expected_blob = pack_profile(expected)
    out_blob = (tmp_path / "compiled.bin").read_bytes()
    assert out_blob == expected_blob
    assert out_blob[96] == 0


def test_load_ir_cube_rejects_invalid_inputs(tmp_path: Path):
    sample_rate = 44100
    sf.write(str(tmp_path / "bad5.wav"), np.zeros((128, 5), dtype=np.float32), sample_rate, subtype="FLOAT")
    sf.write(str(tmp_path / "sr48.wav"), np.zeros((128, 6), dtype=np.float32), 48000, subtype="FLOAT")

    with pytest.raises(ValueError, match="exactly 6 channels"):
        load_ir_cube(
            {
                "schema_version": 1,
                "sample_rate_hz": sample_rate,
                "layout": "multichannel",
                "directions": [{"azimuth_deg": 0.0, "path": "bad5.wav"}],
            },
            tmp_path,
        )

    with pytest.raises(ValueError, match="Sample-rate mismatch"):
        load_ir_cube(
            {
                "schema_version": 1,
                "sample_rate_hz": sample_rate,
                "layout": "multichannel",
                "directions": [{"azimuth_deg": 0.0, "path": "sr48.wav"}],
            },
            tmp_path,
        )

    with pytest.raises(ValueError, match="missing files"):
        load_ir_cube(
            {
                "schema_version": 1,
                "sample_rate_hz": sample_rate,
                "layout": "multichannel",
                "directions": [{"azimuth_deg": 0.0, "path": "missing.wav"}],
            },
            tmp_path,
        )


def test_main_requires_mode_flag(tmp_path: Path):
    with pytest.raises(SystemExit):
        main(["--output-prefix", str(tmp_path / "out")])
