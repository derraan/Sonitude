from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from app.audio_io.stream_io import (
    estimated_pcm_bytes,
    iter_mapped_blocks,
    load_file_overview,
    load_plot_preview,
    should_stream_batch,
)
from app.storage.models import AudioMetadata


def _meta(*, frames: int, channels: int, sample_rate: int) -> AudioMetadata:
    return AudioMetadata(
        path=Path("x.wav"),
        filename="x.wav",
        sample_rate_hz=sample_rate,
        channels=channels,
        bit_depth=16,
        subtype="PCM_16",
        duration_s=frames / sample_rate,
        num_samples=frames,
        container="wav",
    )


def test_short_clip_stays_in_core() -> None:
    meta = _meta(frames=44100, channels=6, sample_rate=44100)
    assert estimated_pcm_bytes(meta) < 24 * 1024 * 1024
    assert should_stream_batch(meta) is False


def test_multi_hour_capture_uses_streaming() -> None:
    meta = _meta(frames=367_524_108, channels=8, sample_rate=44100)
    assert meta.duration_s > 8000
    assert should_stream_batch(meta) is True


def test_iter_mapped_blocks_does_not_require_full_load(tmp_path: Path, sample_rate: int) -> None:
    n = sample_rate * 2
    data = np.stack([np.full(n, float(ch), dtype=np.float32) for ch in range(8)], axis=1)
    path = tmp_path / "eight.wav"
    sf.write(str(path), data, sample_rate, subtype="FLOAT")
    blocks = list(iter_mapped_blocks(path, [0, 1, 2, 3, 4, 5], block_frames=1024))
    joined = np.concatenate(blocks, axis=0)
    assert joined.shape == (n, 6)
    np.testing.assert_allclose(joined[:, 5], 5.0)


def test_plot_preview_caps_length(tmp_path: Path, sample_rate: int) -> None:
    n = 50_000
    data = np.zeros((n, 2), dtype=np.float32)
    data[:, 0] = np.linspace(-1, 1, n, dtype=np.float32)
    path = tmp_path / "long_stereo.wav"
    sf.write(str(path), data, sample_rate, subtype="FLOAT")
    preview, sr = load_plot_preview(path, max_points=200)
    assert sr == sample_rate
    assert len(preview) <= 200
    assert preview.shape[1] == 2


def test_file_overview_uses_true_duration_and_nyquist(tmp_path: Path, sample_rate: int) -> None:
    duration_s = 1.0
    n = int(sample_rate * duration_s)
    t = np.linspace(0, duration_s, n, endpoint=False)
    tone = (0.2 * np.sin(2 * np.pi * 1000 * t)).astype(np.float32)
    data = np.stack([tone, tone], axis=1)
    path = tmp_path / "tone.wav"
    sf.write(str(path), data, sample_rate, subtype="FLOAT")
    overview = load_file_overview(path, n_wave=64, n_spec=32, spec_nperseg=128)
    assert abs(overview.duration_s - duration_s) < 0.02
    assert overview.spectrogram_db is not None
    assert overview.spectrogram_freqs is not None
    assert overview.spectrogram_freqs[-1] == pytest.approx(sample_rate / 2.0)
    assert overview.spectrogram_db.shape[1] == 32
