"""End-to-end integration test against the REAL sonitude_wav_replay binary.

Skipped automatically when the binary hasn't been built (see
testbench/README.md for build instructions) — this is intentional: it lets
the same test suite run meaningfully in an environment with a C++ toolchain
(e.g. CI) while not failing spuriously where there isn't one. This directly
covers what the pure-Python unit tests cannot: that the real algorithm
processes a real 6-channel fixture, and that enabling suppression actually
changes the output.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from app.audio_io import wav_loader
from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.processing.batch_adapter import run_wav_replay
from app.processing.sonitude_binary_locator import MissingBinaryError, find_binary
from app.storage.models import SteeringEvent

try:
    _WAV_REPLAY_BINARY = find_binary("sonitude_wav_replay")
except MissingBinaryError:
    _WAV_REPLAY_BINARY = None

pytestmark = pytest.mark.skipif(
    _WAV_REPLAY_BINARY is None,
    reason="sonitude_wav_replay not built; see testbench/README.md build instructions",
)


@pytest.fixture
def six_channel_fixture(tmp_path: Path) -> Path:
    config_summary = read_runtime_config_summary(DEFAULT_CONFIG_PATH)
    sample_rate = config_summary.capture_sample_rate_hz
    duration_s = 1.0
    n = int(sample_rate * duration_s)
    t = np.linspace(0, duration_s, n, endpoint=False)
    tone = 0.3 * np.sin(2 * np.pi * 400 * t)
    rng = np.random.default_rng(7)
    data = np.stack([tone + rng.normal(0, 0.02, n) for _ in range(6)], axis=1).astype(np.float32)
    path = tmp_path / "fixture_6ch.wav"
    sf.write(str(path), data, sample_rate, subtype="FLOAT")
    return path


def test_wav_replay_produces_all_expected_outputs(six_channel_fixture: Path, tmp_path: Path) -> None:
    events = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0)]
    result = run_wav_replay(
        six_channel_fixture, DEFAULT_CONFIG_PATH, events, tmp_path / "out",
        binary_path=_WAV_REPLAY_BINARY,
    )
    assert result.output_wav.exists()
    assert result.beamformed_wav.exists()
    assert result.suppressed_wav.exists()

    beamformed, sr = wav_loader.load_wav(result.beamformed_wav)
    assert sr == read_runtime_config_summary(DEFAULT_CONFIG_PATH).capture_sample_rate_hz
    assert len(beamformed) > 0
    assert np.any(beamformed != 0.0)


def test_suppression_actually_changes_the_output(six_channel_fixture: Path, tmp_path: Path) -> None:
    events = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0)]

    without_suppression = run_wav_replay(
        six_channel_fixture, DEFAULT_CONFIG_PATH, events, tmp_path / "no_suppression",
        enable_suppression=False, binary_path=_WAV_REPLAY_BINARY,
    )
    with_suppression = run_wav_replay(
        six_channel_fixture, DEFAULT_CONFIG_PATH, events, tmp_path / "with_suppression",
        enable_suppression=True, binary_path=_WAV_REPLAY_BINARY,
    )

    beamformed_a, _ = wav_loader.load_wav(without_suppression.beamformed_wav)
    beamformed_b, _ = wav_loader.load_wav(with_suppression.beamformed_wav)
    np.testing.assert_allclose(beamformed_a, beamformed_b, atol=1e-6)  # pre-suppression tap is identical

    suppressed_without, _ = wav_loader.load_wav(without_suppression.suppressed_wav)
    suppressed_with, _ = wav_loader.load_wav(with_suppression.suppressed_wav)
    assert not np.allclose(suppressed_without, suppressed_with, atol=1e-6), (
        "enabling suppression should change the post-suppression tap relative to suppression disabled"
    )


def test_width_blend_moves_output_toward_omnidirectional(six_channel_fixture: Path, tmp_path: Path) -> None:
    narrow = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0, width_deg=0.0)]
    wide = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0, width_deg=180.0)]

    narrow_result = run_wav_replay(six_channel_fixture, DEFAULT_CONFIG_PATH, narrow, tmp_path / "narrow", binary_path=_WAV_REPLAY_BINARY)
    wide_result = run_wav_replay(six_channel_fixture, DEFAULT_CONFIG_PATH, wide, tmp_path / "wide", binary_path=_WAV_REPLAY_BINARY)

    narrow_beam, _ = wav_loader.load_wav(narrow_result.beamformed_wav)
    wide_beam, _ = wav_loader.load_wav(wide_result.beamformed_wav)
    assert not np.allclose(narrow_beam, wide_beam, atol=1e-6), "width_deg=180 should differ from width_deg=0"
