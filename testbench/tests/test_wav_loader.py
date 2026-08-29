from __future__ import annotations

from pathlib import Path

from app.audio_io import wav_loader


def test_read_wav_metadata(six_channel_wav: Path, sample_rate: int) -> None:
    metadata = wav_loader.read_wav_metadata(six_channel_wav)
    assert metadata.channels == 6
    assert metadata.sample_rate_hz == sample_rate
    assert metadata.num_samples > 0
    assert metadata.duration_s > 0


def test_validate_six_channel_wav_ok(six_channel_wav: Path, sample_rate: int) -> None:
    result = wav_loader.validate_six_channel_wav(six_channel_wav, expected_sample_rate_hz=sample_rate)
    assert result.ok
    assert result.errors == []
    assert result.metadata is not None


def test_validate_rejects_wrong_sample_rate(six_channel_wav: Path) -> None:
    result = wav_loader.validate_six_channel_wav(six_channel_wav, expected_sample_rate_hz=44100)
    assert not result.ok
    assert any("Sample rate" in e for e in result.errors)


def test_validate_rejects_missing_file(tmp_path: Path) -> None:
    result = wav_loader.validate_six_channel_wav(tmp_path / "does_not_exist.wav")
    assert not result.ok
    assert result.errors


def test_load_wav_shape(six_channel_wav: Path, sample_rate: int) -> None:
    data, sr = wav_loader.load_wav(six_channel_wav)
    assert sr == sample_rate
    assert data.shape[1] == 6


def test_extract_channels(six_channel_wav: Path) -> None:
    data, _ = wav_loader.load_wav(six_channel_wav)
    extracted = wav_loader.extract_channels(data, [5, 4, 3, 2, 1, 0])
    assert extracted.shape == data.shape
    assert (extracted[:, 0] == data[:, 5]).all()


def test_find_wav_files(tmp_path: Path, six_channel_wav: Path) -> None:
    # six_channel_wav already lives under a tmp_path-derived dir.
    found = wav_loader.find_wav_files(six_channel_wav.parent)
    assert six_channel_wav in found
