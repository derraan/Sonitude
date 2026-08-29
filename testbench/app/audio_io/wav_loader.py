"""WAV loading, metadata extraction, and validation for the 6-channel pipeline.

This module never touches the algorithm; it only reads/validates files with
``soundfile`` so the GUI can show metadata and reject bad input before spending
time invoking the C++ tools.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf

from app.storage.models import ValidationResult, WavMetadata

REQUIRED_CHANNELS = 6

# soundfile subtype -> effective bit depth, for display purposes only.
_SUBTYPE_BIT_DEPTH = {
    "PCM_16": 16,
    "PCM_24": 24,
    "PCM_32": 32,
    "PCM_U8": 8,
    "FLOAT": 32,
    "DOUBLE": 64,
}


def read_wav_metadata(path: str | Path) -> WavMetadata:
    """Read a WAV file's header/metadata without loading the full sample data."""
    path = Path(path)
    info = sf.info(str(path))
    bit_depth = _SUBTYPE_BIT_DEPTH.get(info.subtype, 0)
    return WavMetadata(
        path=path,
        filename=path.name,
        sample_rate_hz=info.samplerate,
        channels=info.channels,
        bit_depth=bit_depth,
        subtype=info.subtype,
        duration_s=info.frames / info.samplerate if info.samplerate else 0.0,
        num_samples=info.frames,
    )


def validate_six_channel_wav(
    path: str | Path,
    expected_sample_rate_hz: int | None = None,
) -> ValidationResult:
    """Validate that a file is a readable WAV with at least 6 channels.

    ``expected_sample_rate_hz`` should be the runtime config's capture sample
    rate, since ``sonitude_wav_replay`` requires an exact match (no resampling
    is performed anywhere in the pipeline).
    """
    path = Path(path)
    errors: list[str] = []

    if not path.exists():
        return ValidationResult(ok=False, errors=[f"File does not exist: {path}"])
    if path.suffix.lower() != ".wav":
        errors.append(f"Not a .wav file: {path.name}")

    try:
        metadata = read_wav_metadata(path)
    except Exception as exc:  # noqa: BLE001 - surfaced to the user as a validation error
        return ValidationResult(ok=False, errors=[f"Unreadable WAV file: {exc}"])

    if metadata.channels < REQUIRED_CHANNELS:
        errors.append(
            f"Expected at least {REQUIRED_CHANNELS} channels, found {metadata.channels}"
        )
    if metadata.num_samples == 0:
        errors.append("WAV file contains no samples")
    if expected_sample_rate_hz is not None and metadata.sample_rate_hz != expected_sample_rate_hz:
        errors.append(
            "Sample rate mismatch: file is "
            f"{metadata.sample_rate_hz} Hz, pipeline requires exactly "
            f"{expected_sample_rate_hz} Hz (no resampling is performed)"
        )

    return ValidationResult(ok=not errors, errors=errors, metadata=metadata)


def load_wav(path: str | Path) -> tuple[np.ndarray, int]:
    """Load full sample data as float32, shape (num_samples, channels)."""
    data, sample_rate = sf.read(str(path), dtype="float32", always_2d=True)
    return data, sample_rate


def extract_channels(data: np.ndarray, channel_indices: list[int]) -> np.ndarray:
    """Select and reorder channels, e.g. via config's active_channel_map."""
    return data[:, channel_indices]


def find_wav_files(folder: str | Path) -> list[Path]:
    """List .wav files directly inside a folder, sorted by name."""
    folder = Path(folder)
    return sorted(p for p in folder.glob("*.wav") if p.is_file())
