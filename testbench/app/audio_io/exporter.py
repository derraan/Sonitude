"""Lossless result export. Changing container does not change DSP."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf

LOSSLESS_FORMATS = ("wav", "flac")


class ExportError(RuntimeError):
    """Raised when a lossless export cannot be written or read back."""


def export_pcm(
    path: str | Path,
    data: np.ndarray,
    sample_rate_hz: int,
    *,
    container: str = "wav",
    subtype: str | None = None,
) -> Path:
    """Write float32 PCM to WAV or FLAC. MP3 is intentionally unsupported."""
    path = Path(path)
    container = container.lower().lstrip(".")
    if container not in LOSSLESS_FORMATS:
        raise ExportError(f"Unsupported export container {container!r}; use wav or flac (no MP3 output)")
    pcm = np.asarray(data, dtype=np.float32)
    if pcm.ndim == 1:
        pcm = pcm.reshape(-1, 1)
    # WAV keeps float32 unless the caller asks for integer (Qt playback).
    # FLAC does not support IEEE float in libsndfile; PCM_24 is lossless
    # relative to 24-bit integer quantization.
    if subtype is None:
        subtype = "FLOAT" if container == "wav" else "PCM_24"
    try:
        sf.write(str(path), pcm, sample_rate_hz, format=container.upper(), subtype=subtype)
    except Exception as exc:  # noqa: BLE001
        raise ExportError(f"Failed to export {container}: {exc}") from exc
    return path


def transcode_lossless(
    source: str | Path,
    dest: str | Path,
    *,
    container: str,
    subtype: str | None = None,
    block_frames: int = 8192,
) -> Path:
    """Copy PCM into WAV or FLAC without loading the whole file into RAM."""
    source = Path(source)
    dest = Path(dest)
    container = container.lower().lstrip(".")
    if container not in LOSSLESS_FORMATS:
        raise ExportError(f"Unsupported export container {container!r}; use wav or flac (no MP3 output)")
    if subtype is None:
        subtype = "FLOAT" if container == "wav" else "PCM_24"
    try:
        with sf.SoundFile(str(source)) as reader:
            dest.parent.mkdir(parents=True, exist_ok=True)
            with sf.SoundFile(
                str(dest),
                mode="w",
                samplerate=reader.samplerate,
                channels=reader.channels,
                format=container.upper(),
                subtype=subtype,
            ) as writer:
                while True:
                    block = reader.read(block_frames, dtype="float32", always_2d=True)
                    if len(block) == 0:
                        break
                    writer.write(block)
    except ExportError:
        raise
    except Exception as exc:  # noqa: BLE001
        raise ExportError(f"Failed to transcode {source.name} to {container}: {exc}") from exc
    return dest


def read_back(path: str | Path) -> tuple[np.ndarray, int]:
    data, sample_rate = sf.read(str(path), dtype="float32", always_2d=True)
    return data, int(sample_rate)
