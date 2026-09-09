from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import soundfile as sf


@dataclass(frozen=True)
class WavData:
    sample_rate_hz: int
    channels: int
    frames: int
    samples: np.ndarray  # shape [frames, channels], float64


def read_wav(path: str | Path) -> WavData:
    pcm, sr = sf.read(str(path), dtype="float64", always_2d=True)
    if pcm.ndim != 2:
        raise RuntimeError(f"Expected 2D audio array, got shape={pcm.shape} from {path}")
    frames, channels = pcm.shape
    if frames == 0 or channels == 0:
        raise RuntimeError(f"Empty WAV file: {path}")
    return WavData(sample_rate_hz=int(sr), channels=int(channels), frames=int(frames), samples=pcm)


def split_channels(wav: WavData, required_channels: int) -> list[np.ndarray]:
    if wav.channels < required_channels:
        raise RuntimeError(
            f"WAV has {wav.channels} channels but requires at least {required_channels}"
        )
    return [wav.samples[:, idx].copy() for idx in range(required_channels)]
