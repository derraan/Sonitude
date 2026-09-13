from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np


@dataclass
class MetricsResult:
    snr_db: float
    si_sdr_db: float


def _safe_energy(x: np.ndarray, eps: float = 1e-8) -> float:
    return float(np.sum(np.square(x), dtype=np.float64) + eps)


def snr_db(reference: np.ndarray, estimate: np.ndarray, eps: float = 1e-8) -> float:
    reference = np.asarray(reference, dtype=np.float64)
    estimate = np.asarray(estimate, dtype=np.float64)
    noise = reference - estimate
    return 10.0 * math.log10((_safe_energy(reference, eps)) / (_safe_energy(noise, eps)))


def si_sdr_db(reference: np.ndarray, estimate: np.ndarray, eps: float = 1e-8) -> float:
    reference = np.asarray(reference, dtype=np.float64)
    estimate = np.asarray(estimate, dtype=np.float64)
    alpha = float(np.dot(reference, estimate) / (_safe_energy(reference, eps)))
    target = alpha * reference
    noise = target - estimate
    return 10.0 * math.log10((_safe_energy(target, eps)) / (_safe_energy(noise, eps)))


def pair_metrics(reference: np.ndarray, estimate: np.ndarray) -> MetricsResult:
    if reference.shape != estimate.shape:
        n = min(reference.shape[0], estimate.shape[0])
        reference = reference[:n]
        estimate = estimate[:n]
    return MetricsResult(snr_db=snr_db(reference, estimate), si_sdr_db=si_sdr_db(reference, estimate))
