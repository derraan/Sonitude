"""Adversarial tests for audio loading: codec success is not DSP validity."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from app.audio_io import audio_loader
from app.audio_io.audio_loader import CodecCapabilityError, CodecError

STEREO_MP3 = Path(__file__).resolve().parent / "fixtures" / "stereo_tone.mp3"


def test_read_wav_metadata(six_channel_wav: Path, sample_rate: int) -> None:
    metadata = audio_loader.read_wav_metadata(six_channel_wav)
    assert metadata.channels == 6
    assert metadata.sample_rate_hz == sample_rate
    assert metadata.num_samples > 0
    assert metadata.duration_s > 0
    assert metadata.container == "wav"


def test_validate_six_channel_wav_ok(six_channel_wav: Path, sample_rate: int) -> None:
    result = audio_loader.validate_six_channel_wav(six_channel_wav, expected_sample_rate_hz=sample_rate)
    assert result.ok
    assert result.errors == []
    assert result.metadata is not None


def test_validate_rejects_wrong_sample_rate(six_channel_wav: Path) -> None:
    result = audio_loader.validate_six_channel_wav(six_channel_wav, expected_sample_rate_hz=44100)
    assert not result.ok
    assert result.error_kind == "dsp_input"
    assert any("Sample rate" in e for e in result.errors)


def test_validate_rejects_missing_file(tmp_path: Path) -> None:
    result = audio_loader.validate_six_channel_wav(tmp_path / "does_not_exist.wav")
    assert not result.ok
    assert result.errors


def test_load_wav_shape(six_channel_wav: Path, sample_rate: int) -> None:
    data, sr = audio_loader.load_wav(six_channel_wav)
    assert sr == sample_rate
    assert data.shape[1] == 6
    assert data.dtype == np.float32


def test_extract_channels(six_channel_wav: Path) -> None:
    data, _ = audio_loader.load_wav(six_channel_wav)
    extracted = audio_loader.extract_channels(data, [5, 4, 3, 2, 1, 0])
    assert extracted.shape == data.shape
    assert (extracted[:, 0] == data[:, 5]).all()


def test_find_wav_files(tmp_path: Path, six_channel_wav: Path) -> None:
    found = audio_loader.find_wav_files(six_channel_wav.parent)
    assert six_channel_wav in found


def test_stereo_wav_decodes_but_fails_dsp_validation(stereo_wav: Path, sample_rate: int) -> None:
    data, metadata = audio_loader.load_audio(stereo_wav)
    assert data.shape[1] == 2
    assert metadata.channels == 2
    result = audio_loader.validate_dsp_input(data, expected_sample_rate_hz=sample_rate)
    assert not result.ok
    assert any("2 channel" in e for e in result.errors)


def test_stereo_flac_decodes_but_fails_dsp_validation(stereo_flac: Path, sample_rate: int) -> None:
    data, metadata = audio_loader.load_audio(stereo_flac)
    assert metadata.container == "flac"
    assert data.shape[1] == 2
    file_result = audio_loader.validate_audio_file(stereo_flac, expected_sample_rate_hz=sample_rate)
    assert file_result.error_kind == "dsp_input"
    assert not file_result.ok


def test_six_channel_flac_is_valid_dsp_input(six_channel_flac: Path, sample_rate: int) -> None:
    data, metadata = audio_loader.load_audio(six_channel_flac)
    assert metadata.container == "flac"
    assert data.shape[1] == 6
    result = audio_loader.validate_audio_file(six_channel_flac, expected_sample_rate_hz=sample_rate)
    assert result.ok


def test_does_not_silently_downmix(stereo_wav: Path) -> None:
    data, _ = audio_loader.load_audio(stereo_wav)
    assert data.ndim == 2
    assert data.shape[1] == 2


def test_channel_map_out_of_range_fails_dsp_not_codec(six_channel_wav: Path, sample_rate: int) -> None:
    result = audio_loader.validate_audio_file(
        six_channel_wav,
        expected_sample_rate_hz=sample_rate,
        active_channel_map=[0, 1, 2, 3, 4, 9],
    )
    assert result.error_kind == "dsp_input"
    assert not result.ok


def test_mp3_capability_error_when_unsupported(tmp_path: Path) -> None:
    fake = tmp_path / "speech.mp3"
    fake.write_bytes(b"not a real mp3")
    if audio_loader.mp3_decode_supported():
        with pytest.raises(CodecError):
            audio_loader.load_audio(fake)
    else:
        with pytest.raises(CodecCapabilityError, match="MP3 decode is not available"):
            audio_loader.load_audio(fake)


def test_valid_stereo_mp3_decodes_and_preserves_channels(sample_rate: int) -> None:
    if not audio_loader.mp3_decode_supported():
        pytest.skip("installed libsndfile cannot decode MP3")
    assert STEREO_MP3.exists(), "commit testbench/tests/fixtures/stereo_tone.mp3"
    data, metadata = audio_loader.load_audio(STEREO_MP3)
    assert metadata.container == "mp3"
    assert metadata.channels == 2
    assert data.shape[1] == 2
    assert data.dtype == np.float32
    assert metadata.sample_rate_hz == sample_rate
    assert data.shape[0] == metadata.num_samples
    assert np.all(np.isfinite(data))
    assert np.max(np.abs(data)) > 1e-3
    result = audio_loader.validate_audio_file(
        STEREO_MP3,
        expected_sample_rate_hz=sample_rate,
        active_channel_map=[0, 1, 2, 3, 4, 5],
    )
    assert result.error_kind == "dsp_input"
    assert not result.ok
    assert any("Decoded successfully with 2 channel" in e for e in result.errors)


def test_corrupt_wav_is_codec_error_not_dsp(tmp_path: Path) -> None:
    path = tmp_path / "corrupt.wav"
    path.write_bytes(b"RIFF    WAVEfmt not-audio")
    result = audio_loader.validate_audio_file(path)
    assert not result.ok
    assert result.error_kind in {"codec", "codec_capability"}
