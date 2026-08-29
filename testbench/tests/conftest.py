from __future__ import annotations

import os
import sys
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

TESTBENCH_ROOT = Path(__file__).resolve().parents[1]
if str(TESTBENCH_ROOT) not in sys.path:
    sys.path.insert(0, str(TESTBENCH_ROOT))

from app.processing.sonitude_binary_locator import MissingBinaryError, find_binary


def require_cpp_binary(tool_name: str) -> Path:
    """Locate a C++ tool. Fail (do not skip) when SONITUDE_REQUIRE_CPP=1."""
    try:
        return find_binary(tool_name)
    except MissingBinaryError as exc:
        if os.environ.get("SONITUDE_REQUIRE_CPP") == "1":
            pytest.fail(f"SONITUDE_REQUIRE_CPP=1 but {tool_name} was not found: {exc}")
        pytest.skip(str(exc))


@pytest.fixture(scope="session")
def wav_replay_binary() -> Path:
    return require_cpp_binary("sonitude_wav_replay")


@pytest.fixture(scope="session")
def stream_process_binary() -> Path:
    return require_cpp_binary("sonitude_stream_process")


@pytest.fixture
def sample_rate() -> int:
    return 16000


@pytest.fixture
def six_channel_wav(tmp_path: Path, sample_rate: int) -> Path:
    """A short synthetic 6-channel WAV file for loader/validation tests."""
    duration_s = 0.5
    t = np.linspace(0, duration_s, int(sample_rate * duration_s), endpoint=False)
    tone = 0.2 * np.sin(2 * np.pi * 440 * t)
    data = np.stack([tone * (0.5 + 0.1 * ch) for ch in range(6)], axis=1).astype(np.float32)
    path = tmp_path / "six_channel.wav"
    sf.write(str(path), data, sample_rate, subtype="FLOAT")
    return path


@pytest.fixture
def stereo_wav(tmp_path: Path, sample_rate: int) -> Path:
    n = int(sample_rate * 0.2)
    t = np.linspace(0, 0.2, n, endpoint=False)
    left = (0.2 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    right = (0.2 * np.sin(2 * np.pi * 550 * t)).astype(np.float32)
    path = tmp_path / "stereo.wav"
    sf.write(str(path), np.stack([left, right], axis=1), sample_rate, subtype="FLOAT")
    return path


@pytest.fixture
def six_channel_flac(tmp_path: Path, sample_rate: int) -> Path:
    n = int(sample_rate * 0.2)
    t = np.linspace(0, 0.2, n, endpoint=False)
    tone = 0.2 * np.sin(2 * np.pi * 440 * t)
    data = np.stack([tone * (0.5 + 0.1 * ch) for ch in range(6)], axis=1).astype(np.float32)
    path = tmp_path / "six_channel.flac"
    sf.write(str(path), data, sample_rate, format="FLAC", subtype="PCM_24")
    return path


@pytest.fixture
def stereo_flac(tmp_path: Path, sample_rate: int) -> Path:
    n = int(sample_rate * 0.2)
    t = np.linspace(0, 0.2, n, endpoint=False)
    left = (0.2 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    right = (0.2 * np.sin(2 * np.pi * 550 * t)).astype(np.float32)
    path = tmp_path / "stereo.flac"
    sf.write(str(path), np.stack([left, right], axis=1), sample_rate, format="FLAC", subtype="PCM_24")
    return path


@pytest.fixture
def mono_signal_pair(sample_rate: int) -> tuple[np.ndarray, np.ndarray]:
    """A synthetic (noisy_before, cleaner_after) mono pair for metric tests."""
    duration_s = 2.0
    n = int(sample_rate * duration_s)
    rng = np.random.default_rng(42)
    t = np.linspace(0, duration_s, n, endpoint=False)
    tone = 0.3 * np.sin(2 * np.pi * 500 * t)

    gate_period_s = 0.2
    duty_cycle = 0.6
    phase = (t % gate_period_s) / gate_period_s
    envelope = (phase < duty_cycle).astype(np.float32)
    gated_tone = tone * envelope

    noise_before = rng.normal(0, 0.15, n)
    noise_after = rng.normal(0, 0.02, n)
    before = (gated_tone + noise_before).astype(np.float32)
    after = (gated_tone + noise_after).astype(np.float32)
    return before, after
