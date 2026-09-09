from __future__ import annotations

import numpy as np

from tools.calibration.estimate_delay import estimate_delays_by_band
from tools.calibration.estimate_gain import estimate_gain_linear
from tools.calibration.geometry import REFERENCE_INDEX


def _delay_read_linear(x: np.ndarray, delay_samples: float) -> np.ndarray:
    n = np.arange(x.shape[0], dtype=np.float64) - delay_samples
    n0 = np.floor(n).astype(int)
    frac = n - n0
    n0 = np.clip(n0, 0, x.shape[0] - 1)
    n1 = np.clip(n0 + 1, 0, x.shape[0] - 1)
    return ((1.0 - frac) * x[n0]) + (frac * x[n1])


def test_delay_estimate_roundtrip():
    fs = 44100
    n = 32768
    rng = np.random.default_rng(42)
    base = rng.normal(0.0, 0.2, size=n)
    mismatch = np.array([0.0, 4.0, -3.0, 2.0, -5.0, 3.0], dtype=np.float64)
    chans = [_delay_read_linear(base, d) for d in mismatch]

    by_band = estimate_delays_by_band(chans, fs, reference_index=REFERENCE_INDEX, max_lag_samples=16)
    lags = np.array([d.lag_samples for d in by_band["300_8000"]], dtype=np.float64)
    expected = mismatch - mismatch[REFERENCE_INDEX]
    assert abs(lags[REFERENCE_INDEX]) < 0.25
    assert np.max(np.abs(np.abs(lags) - np.abs(expected))) < 1.25


def test_gain_estimate_with_known_scalars():
    fs = 44100
    n = 32768
    t = np.arange(n) / fs
    ref = np.sin(2.0 * np.pi * 1200.0 * t)
    scale = np.array([1.0, 0.8, 1.1, 1.3, 0.7, 0.9], dtype=np.float64)
    chans = [ref * s for s in scale]

    est = estimate_gain_linear(chans, fs, reference_index=REFERENCE_INDEX)
    gains = np.array([e.gain_linear for e in est], dtype=np.float64)
    assert np.all(gains > 0.0)
    assert np.all(gains <= 8.0)
    # Relative ratios should trend inverse to original scale.
    assert gains[3] < gains[4]


def test_delay_closure_reduces_residual():
    fs = 44100
    n = 32768
    t = np.arange(n) / fs
    base = np.sin(2.0 * np.pi * 1000.0 * t)
    mismatch = np.array([0.0, 1.0, -0.5, 0.2, -1.2, 0.8], dtype=np.float64)
    chans = [_delay_read_linear(base, d) for d in mismatch]
    by_band = estimate_delays_by_band(chans, fs, reference_index=REFERENCE_INDEX, max_lag_samples=16)
    lags = np.array([d.lag_samples for d in by_band["300_8000"]], dtype=np.float64)

    corrected = [_delay_read_linear(chans[i], -lags[i]) for i in range(6)]
    by_band_after = estimate_delays_by_band(corrected, fs, reference_index=REFERENCE_INDEX, max_lag_samples=16)
    residual = np.array([d.lag_samples for d in by_band_after["300_8000"]], dtype=np.float64)
    assert np.max(np.abs(residual)) < np.max(np.abs(lags))
