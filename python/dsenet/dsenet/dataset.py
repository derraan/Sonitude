from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np


@dataclass(frozen=True)
class Geometry:
    mic_positions_xyz_m: np.ndarray  # [M,3]


@dataclass(frozen=True)
class DatasetConfig:
    sample_rate_hz: int = 16000
    duration_s: float = 4.0
    sigma: float = 0.2
    rho: float = 8.0
    target_azimuth_min_deg: float = -10.0
    target_azimuth_max_deg: float = 10.0


def beta_from_azimuth_rad(theta_rad: np.ndarray, sigma: float, rho: float) -> np.ndarray:
    return np.exp(-0.5 * np.power(theta_rad / sigma, rho))


def paper_target_from_reference_signals(
    reference_mic_signals: np.ndarray, source_azimuth_rad: np.ndarray, sigma: float, rho: float
) -> np.ndarray:
    """
    Eq. (2)-(3) target from already-simulated reverberant source components at ref mic:
      z = sum_i beta(theta_i) * x_{1,i}
    reference_mic_signals: [num_sources, num_samples]
    """
    beta = beta_from_azimuth_rad(source_azimuth_rad, sigma=sigma, rho=rho).astype(np.float32)
    return np.sum(reference_mic_signals * beta[:, None], axis=0).astype(np.float32)


def load_librispeech_filelist(root: Path) -> list[Path]:
    return sorted(root.rglob("*.flac"))


def generate_pairs(filelist: Iterable[Path]) -> Iterable[tuple[Path, Path]]:
    files = list(filelist)
    for i in range(0, len(files) - 1, 2):
        yield files[i], files[i + 1]
