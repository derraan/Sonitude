from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import signal


@dataclass(frozen=True)
class DelayBandEstimate:
    lag_samples: float
    peak_abs_corr: float
    peak_signed_corr: float


BANDS_HZ: dict[str, tuple[float, float]] = {
    "300_3000": (300.0, 3000.0),
    "3000_8000": (3000.0, 8000.0),
    "300_8000": (300.0, 8000.0),
}


def _bandpass(x: np.ndarray, fs: int, low_hz: float, high_hz: float) -> np.ndarray:
    nyq = 0.5 * float(fs)
    low = max(1.0, low_hz) / nyq
    high = min(high_hz, nyq * 0.99) / nyq
    if not (0.0 < low < high < 1.0):
        raise RuntimeError(f"Invalid bandpass bounds low={low_hz} high={high_hz} for fs={fs}")
    b, a = signal.butter(4, [low, high], btype="bandpass")
    return signal.filtfilt(b, a, x).astype(np.float64, copy=False)


def _parabolic_peak(y0: float, y1: float, y2: float) -> float:
    denom = y0 - (2.0 * y1) + y2
    if abs(denom) < 1.0e-12:
        return 0.0
    return 0.5 * (y0 - y2) / denom


def gcc_phat_lag(
    ref: np.ndarray,
    ch: np.ndarray,
    fs: int,
    max_lag_samples: int,
) -> DelayBandEstimate:
    n = int(2 ** np.ceil(np.log2(len(ref) + len(ch))))
    ref_fft = np.fft.rfft(ref, n=n)
    ch_fft = np.fft.rfft(ch, n=n)
    cpsd = ref_fft * np.conj(ch_fft)
    denom = np.abs(cpsd)
    denom = np.maximum(denom, 1.0e-12)
    phat = cpsd / denom
    corr = np.fft.irfft(phat, n=n)

    # Rearrange circular lag axis to signed lags.
    corr = np.concatenate((corr[-max_lag_samples:], corr[: max_lag_samples + 1]))
    lags = np.arange(-max_lag_samples, max_lag_samples + 1, dtype=np.float64)

    idx = int(np.argmax(np.abs(corr)))
    lag = lags[idx]
    peak_signed = float(corr[idx])
    peak_abs = float(abs(peak_signed))
    if 0 < idx < len(corr) - 1:
        frac = _parabolic_peak(abs(float(corr[idx - 1])), abs(float(corr[idx])), abs(float(corr[idx + 1])))
        lag += frac
    return DelayBandEstimate(lag_samples=float(lag), peak_abs_corr=peak_abs, peak_signed_corr=peak_signed)


def estimate_delays_by_band(
    channels: list[np.ndarray],
    sample_rate_hz: int,
    reference_index: int,
    max_lag_samples: int = 100,
) -> dict[str, list[DelayBandEstimate]]:
    ref_raw = channels[reference_index]
    out: dict[str, list[DelayBandEstimate]] = {}
    for band_name, (f0, f1) in BANDS_HZ.items():
        ref = _bandpass(ref_raw, sample_rate_hz, f0, f1)
        band_results: list[DelayBandEstimate] = []
        for ch in channels:
            ch_bp = _bandpass(ch, sample_rate_hz, f0, f1)
            band_results.append(gcc_phat_lag(ref, ch_bp, sample_rate_hz, max_lag_samples=max_lag_samples))
        out[band_name] = band_results
    return out
