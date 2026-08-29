"""Format-independent audio file loading for the test bench.

Decodes WAV/FLAC/MP3 (where libsndfile supports them) to canonical float32
PCM with shape (frames, channels). Does not downmix, does not run DSP, and
does not treat a successful codec decode as a valid six-microphone input.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import soundfile as sf

from app.storage.models import AudioMetadata, ValidationResult

REQUIRED_MIC_CHANNELS = 6
SUPPORTED_INPUT_EXTENSIONS = {".wav", ".flac", ".mp3"}

_SUBTYPE_BIT_DEPTH = {
    "PCM_16": 16,
    "PCM_24": 24,
    "PCM_32": 32,
    "PCM_U8": 8,
    "FLOAT": 32,
    "DOUBLE": 64,
    "MPEG_LAYER_III": 0,
}


class CodecError(RuntimeError):
    """Raised when a file cannot be decoded as audio (as opposed to DSP validation)."""


class CodecCapabilityError(CodecError):
    """Raised when the installed libsndfile cannot decode this container/codec."""


def available_input_formats() -> set[str]:
    """Upper-case libsndfile format names reported by the installed library."""
    try:
        return {name.upper() for name in sf.available_formats()}
    except Exception:  # noqa: BLE001
        return set()


def mp3_decode_supported() -> bool:
    formats = available_input_formats()
    return any(token in formats for token in ("MP3", "MPEG", "MPEG1", "MPEG2"))


def flac_decode_supported() -> bool:
    return "FLAC" in available_input_formats()


def _extension(path: Path) -> str:
    return path.suffix.lower()


def _require_codec_capability(path: Path) -> None:
    ext = _extension(path)
    if ext == ".mp3" and not mp3_decode_supported():
        raise CodecCapabilityError(
            f"MP3 decode is not available from the installed libsndfile "
            f"(available formats: {sorted(available_input_formats()) or 'unknown'}). "
            "No FFmpeg fallback is configured."
        )
    if ext == ".flac" and not flac_decode_supported():
        raise CodecCapabilityError(
            f"FLAC decode is not available from the installed libsndfile "
            f"(available formats: {sorted(available_input_formats()) or 'unknown'})."
        )


def probe_audio(path: str | Path) -> AudioMetadata:
    """Read container metadata without loading sample data."""
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(path)
    _require_codec_capability(path)
    try:
        info = sf.info(str(path))
    except Exception as exc:  # noqa: BLE001
        raise CodecError(f"Unreadable audio file {path.name}: {exc}") from exc
    bit_depth = _SUBTYPE_BIT_DEPTH.get(info.subtype, 0)
    return AudioMetadata(
        path=path,
        filename=path.name,
        container=_extension(path).lstrip("."),
        subtype=info.subtype,
        sample_rate_hz=info.samplerate,
        channels=info.channels,
        bit_depth=bit_depth,
        duration_s=info.frames / info.samplerate if info.samplerate else 0.0,
        num_samples=info.frames,
        decoder="libsndfile/soundfile",
    )


def load_audio(path: str | Path) -> tuple[np.ndarray, AudioMetadata]:
    """Decode to float32 PCM, shape (frames, channels), original channel order."""
    path = Path(path)
    metadata = probe_audio(path)
    try:
        data, sample_rate = sf.read(str(path), dtype="float32", always_2d=True)
    except Exception as exc:  # noqa: BLE001
        raise CodecError(f"Failed to decode {path.name}: {exc}") from exc
    if sample_rate != metadata.sample_rate_hz:
        raise CodecError(f"Sample-rate mismatch while decoding {path.name}")
    if data.shape[1] != metadata.channels:
        raise CodecError(f"Channel-count mismatch while decoding {path.name}")
    if data.size and not np.all(np.isfinite(data)):
        raise CodecError(f"Decoded PCM contains NaN/Inf: {path.name}")
    metadata.num_samples = int(data.shape[0])
    metadata.duration_s = metadata.num_samples / metadata.sample_rate_hz if metadata.sample_rate_hz else 0.0
    return data.astype(np.float32, copy=False), metadata


def validate_dsp_input(
    audio: np.ndarray | AudioMetadata,
    *,
    expected_sample_rate_hz: int | None = None,
    actual_sample_rate_hz: int | None = None,
    active_channel_map: list[int] | None = None,
    required_channels: int = REQUIRED_MIC_CHANNELS,
) -> ValidationResult:
    """Validate decoded audio (or metadata) as a Sonitude microphone-array input.

    A stereo file that decoded successfully still fails here when the active
    configuration requires six microphone channels.
    """
    errors: list[str] = []
    if isinstance(audio, AudioMetadata):
        metadata = audio
        frames = metadata.num_samples
        channels = metadata.channels
        sample_rate_hz = metadata.sample_rate_hz
    else:
        if audio.ndim != 2:
            return ValidationResult(ok=False, errors=["Decoded audio must have shape (frames, channels)"], error_kind="dsp_input")
        frames, channels = audio.shape
        sample_rate_hz = actual_sample_rate_hz
        metadata = None

    if channels < required_channels:
        errors.append(
            f"Decoded successfully with {channels} channel(s), but this configuration "
            f"requires at least {required_channels} microphone channels (not downmixed)."
        )
    if frames == 0:
        errors.append("Audio contains no samples")
    if expected_sample_rate_hz is not None and sample_rate_hz is not None and sample_rate_hz != expected_sample_rate_hz:
        errors.append(
            "Sample rate mismatch: file is "
            f"{sample_rate_hz} Hz, pipeline requires exactly "
            f"{expected_sample_rate_hz} Hz (no resampling is performed)"
        )
    if active_channel_map:
        if len(active_channel_map) != required_channels:
            errors.append(f"active_channel_map must contain exactly {required_channels} indices")
        required_device_channels = max(active_channel_map) + 1 if active_channel_map else required_channels
        if channels < required_device_channels:
            errors.append(
                f"active_channel_map {active_channel_map} requires at least "
                f"{required_device_channels} source channels, file has {channels}"
            )
        if any(index < 0 for index in active_channel_map):
            errors.append("active_channel_map contains a negative channel index")
    return ValidationResult(ok=not errors, errors=errors, metadata=metadata, error_kind="dsp_input" if errors else None)


def validate_audio_file(
    path: str | Path,
    *,
    expected_sample_rate_hz: int | None = None,
    active_channel_map: list[int] | None = None,
) -> ValidationResult:
    """Probe a file, then run DSP-input validation. Codec failures stay CodecError."""
    path = Path(path)
    if not path.exists():
        return ValidationResult(ok=False, errors=[f"File does not exist: {path}"])
    try:
        metadata = probe_audio(path)
    except CodecCapabilityError as exc:
        return ValidationResult(ok=False, errors=[str(exc)], error_kind="codec_capability")
    except CodecError as exc:
        return ValidationResult(ok=False, errors=[str(exc)], error_kind="codec")
    result = validate_dsp_input(
        metadata,
        expected_sample_rate_hz=expected_sample_rate_hz,
        active_channel_map=active_channel_map,
    )
    result.metadata = metadata
    if not result.ok and result.error_kind is None:
        result.error_kind = "dsp_input"
    if result.ok:
        result.error_kind = None
    return result


def extract_channels(data: np.ndarray, channel_indices: list[int]) -> np.ndarray:
    """Select and reorder channels, e.g. via config's active_channel_map."""
    return data[:, channel_indices]


def find_audio_files(folder: str | Path) -> list[Path]:
    """List supported audio files directly inside a folder, sorted by name."""
    folder = Path(folder)
    return sorted(
        p for p in folder.iterdir()
        if p.is_file() and p.suffix.lower() in SUPPORTED_INPUT_EXTENSIONS
    )


# Compatibility aliases used by older call sites / tests.
WavMetadata = AudioMetadata
REQUIRED_CHANNELS = REQUIRED_MIC_CHANNELS
read_wav_metadata = probe_audio
validate_six_channel_wav = validate_audio_file
find_wav_files = find_audio_files


def load_wav(path: str | Path) -> tuple[np.ndarray, int]:
    data, metadata = load_audio(path)
    return data, metadata.sample_rate_hz
