"""Absolute time-of-flight estimation against a known stimulus.

Correlate / GCC-PHAT each mic channel against the played stimulus (ESS, MLS, …).
Peak lag is absolute TOF in samples from stimulus time zero to arrival at that mic
(including acoustic path and common system delay). Relative mic skew is then
``tof_i - tof_ref``.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .estimate_delay import BANDS_HZ, DelayBandEstimate, _bandpass, _parabolic_peak


@dataclass(frozen=True)
class AbsoluteTofEstimate:
    tof_samples: float
    peak_abs_corr: float
    peak_signed_corr: float

    def as_delay_band(self) -> DelayBandEstimate:
        # lag_samples here means absolute TOF (stimulus -> mic), not mic-vs-mic.
        return DelayBandEstimate(
            lag_samples=float(self.tof_samples),
            peak_abs_corr=float(self.peak_abs_corr),
            peak_signed_corr=float(self.peak_signed_corr),
        )


def default_max_tof_samples(sample_rate_hz: int, distance_m: float = 1.0, speed_of_sound_mps: float = 343.0) -> int:
    """Acoustic TOF at distance plus generous system/alignment margin (~100 ms)."""
    acoustic = (max(distance_m, 0.0) / max(speed_of_sound_mps, 1.0)) * float(sample_rate_hz)
    margin = 0.100 * float(sample_rate_hz)
    return max(256, int(np.ceil(acoustic + margin)))


def gcc_phat_tof(
    stimulus: np.ndarray,
    mic: np.ndarray,
    *,
    max_tof_samples: int,
    min_tof_samples: int = 0,
) -> AbsoluteTofEstimate:
    """Cross-correlate ``mic`` against ``stimulus`` (positive lag => mic later / TOF).

    Uses a matched-filter style spectrum ``mic * conj(stimulus)`` rather than mic-vs-mic
    PHAT: the stimulus is known, so whitening against an unknown source is the wrong tool.
    """
    if max_tof_samples < min_tof_samples:
        raise RuntimeError("max_tof_samples must be >= min_tof_samples")
    n = int(2 ** np.ceil(np.log2(len(stimulus) + len(mic))))
    stim_fft = np.fft.rfft(stimulus, n=n)
    mic_fft = np.fft.rfft(mic, n=n)
    # Peak at +D when mic(t) ≈ stimulus(t - D).
    corr = np.fft.irfft(mic_fft * np.conj(stim_fft), n=n)

    pos = corr[: max_tof_samples + 1].copy()
    if min_tof_samples < 0:
        neg_count = -min_tof_samples
        neg = corr[-neg_count:]
        window = np.concatenate((neg, pos))
        lags = np.arange(min_tof_samples, max_tof_samples + 1, dtype=np.float64)
    else:
        window = pos[min_tof_samples:]
        lags = np.arange(min_tof_samples, max_tof_samples + 1, dtype=np.float64)

    idx = int(np.argmax(np.abs(window)))
    lag = float(lags[idx])
    peak_signed = float(window[idx])
    peak_abs = float(abs(peak_signed))
    if 0 < idx < len(window) - 1:
        frac = _parabolic_peak(abs(float(window[idx - 1])), abs(float(window[idx])), abs(float(window[idx + 1])))
        lag += frac
    return AbsoluteTofEstimate(tof_samples=float(lag), peak_abs_corr=peak_abs, peak_signed_corr=peak_signed)


def estimate_absolute_tof_by_band(
    stimulus: np.ndarray,
    channels: list[np.ndarray],
    sample_rate_hz: int,
    *,
    max_tof_samples: int,
    min_tof_samples: int = 0,
) -> dict[str, list[AbsoluteTofEstimate]]:
    out: dict[str, list[AbsoluteTofEstimate]] = {}
    for band_name, (f0, f1) in BANDS_HZ.items():
        stim_bp = _bandpass(stimulus, sample_rate_hz, f0, f1)
        rows: list[AbsoluteTofEstimate] = []
        for ch in channels:
            ch_bp = _bandpass(ch, sample_rate_hz, f0, f1)
            rows.append(
                gcc_phat_tof(
                    stim_bp,
                    ch_bp,
                    max_tof_samples=max_tof_samples,
                    min_tof_samples=min_tof_samples,
                )
            )
        out[band_name] = rows
    return out


def relative_lags_from_tof(
    tofs: list[AbsoluteTofEstimate],
    reference_index: int,
) -> list[DelayBandEstimate]:
    """Convert absolute TOFs into mic-vs-ref lags (same sign as relative GCC-PHAT)."""
    ref = tofs[reference_index].tof_samples
    out: list[DelayBandEstimate] = []
    for est in tofs:
        lag = float(est.tof_samples - ref)
        out.append(
            DelayBandEstimate(
                lag_samples=lag,
                peak_abs_corr=float(est.peak_abs_corr),
                peak_signed_corr=float(est.peak_signed_corr),
            )
        )
    return out
