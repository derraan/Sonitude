from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import signal


@dataclass(frozen=True)
class GainEstimate:
    median_diff_db: float
    gain_linear: float
    rms_ratio: float


def _bandpass(x: np.ndarray, fs: int, low_hz: float = 300.0, high_hz: float = 8000.0) -> np.ndarray:
    nyq = 0.5 * float(fs)
    b, a = signal.butter(4, [max(1.0, low_hz) / nyq, min(high_hz, nyq * 0.99) / nyq], btype="bandpass")
    return signal.filtfilt(b, a, x).astype(np.float64, copy=False)


def _rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float64), dtype=np.float64)))


def estimate_gain_linear(
    channels: list[np.ndarray], sample_rate_hz: int, reference_index: int
) -> list[GainEstimate]:
    filtered = [_bandpass(ch, sample_rate_hz) for ch in channels]
    ref_rms = max(_rms(filtered[reference_index]), 1.0e-12)

    raw: list[GainEstimate] = []
    for ch in filtered:
        rms = max(_rms(ch), 1.0e-12)
        diff_db = 20.0 * np.log10(rms / ref_rms)
        gain = float(10.0 ** (-diff_db / 20.0))
        raw.append(GainEstimate(median_diff_db=float(diff_db), gain_linear=gain, rms_ratio=(rms / ref_rms)))

    # Keep global loudness unchanged: normalize gains by geometric mean.
    gains = np.array([g.gain_linear for g in raw], dtype=np.float64)
    gmean = float(np.exp(np.mean(np.log(np.maximum(gains, 1.0e-12)))))
    norm = gains / max(gmean, 1.0e-12)
    out: list[GainEstimate] = []
    for idx, g in enumerate(raw):
        out.append(
            GainEstimate(
                median_diff_db=g.median_diff_db,
                gain_linear=float(np.clip(norm[idx], 1.0e-6, 8.0)),
                rms_ratio=g.rms_ratio,
            )
        )
    return out
