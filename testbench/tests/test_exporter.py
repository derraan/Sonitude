"""Lossless WAV/FLAC export. Changing container must not invent DSP."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from app.audio_io.exporter import ExportError, export_pcm, read_back


def test_wav_float32_roundtrip_matches_pcm(tmp_path: Path, sample_rate: int) -> None:
    rng = np.random.default_rng(11)
    original = rng.normal(0, 0.2, (1024, 2)).astype(np.float32)
    path = export_pcm(tmp_path / "out.wav", original, sample_rate, container="wav")
    decoded, sr = read_back(path)
    assert sr == sample_rate
    np.testing.assert_allclose(decoded, original, atol=0, rtol=0)


def test_flac_roundtrip_compares_decoded_pcm(tmp_path: Path, sample_rate: int) -> None:
    rng = np.random.default_rng(12)
    original = rng.normal(0, 0.2, (2048, 1)).astype(np.float32)
    path = export_pcm(tmp_path / "out.flac", original, sample_rate, container="flac")
    decoded, sr = read_back(path)
    assert sr == sample_rate
    assert decoded.shape[1] == 1
    # FLAC is PCM_24: compare the actual decoded PCM against 24-bit quantization
    # of the source, not a file-hash. This fails if we accidentally wrote zeros
    # or a different signal.
    quantized = np.round(original * (2**23)) / (2**23)
    np.testing.assert_allclose(decoded, quantized, atol=2 / (2**23))
    assert not np.allclose(decoded, np.zeros_like(decoded))


def test_mp3_export_is_rejected(tmp_path: Path, sample_rate: int) -> None:
    with pytest.raises(ExportError, match="no MP3"):
        export_pcm(tmp_path / "out.mp3", np.zeros((8, 1), dtype=np.float32), sample_rate, container="mp3")
