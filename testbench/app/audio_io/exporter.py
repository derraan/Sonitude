"""Lossless result export. Changing container does not change DSP."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf

LOSSLESS_FORMATS = ("wav", "flac")


class ExportError(RuntimeError):
    """Raised when a lossless export cannot be written or read back."""


def export_pcm(path: str | Path, data: np.ndarray, sample_rate_hz: int, *, container: str = "wav") -> Path:
    """Write float32 PCM to WAV or FLAC. MP3 is intentionally unsupported."""
    path = Path(path)
    container = container.lower().lstrip(".")
    if container not in LOSSLESS_FORMATS:
        raise ExportError(f"Unsupported export container {container!r}; use wav or flac (no MP3 output)")
    pcm = np.asarray(data, dtype=np.float32)
    if pcm.ndim == 1:
        pcm = pcm.reshape(-1, 1)
    # WAV keeps float32. FLAC does not support IEEE float in libsndfile;
    # PCM_24 is lossless relative to 24-bit integer quantization.
    subtype = "FLOAT" if container == "wav" else "PCM_24"
    try:
        sf.write(str(path), pcm, sample_rate_hz, format=container.upper(), subtype=subtype)
    except Exception as exc:  # noqa: BLE001
        raise ExportError(f"Failed to export {container}: {exc}") from exc
    return path


def read_back(path: str | Path) -> tuple[np.ndarray, int]:
    data, sample_rate = sf.read(str(path), dtype="float32", always_2d=True)
    return data, int(sample_rate)
