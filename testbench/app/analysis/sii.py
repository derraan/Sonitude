"""Approximate Speech Intelligibility Index (SII).

WARNING: this is a SIMPLIFIED, APPROXIMATE estimate, not a certified
implementation of ANSI/ASA S3.5-1997. It uses the standard's general idea
(sum of per-band importance weights times a per-band audibility function
derived from SNR) but omits the full procedure's level-distortion and
upward-spread-of-masking corrections. Every value this module returns is
tagged with ``method`` and should be displayed as "Approximate SII", never
as an official SII figure. Do not use these numbers for any certified,
regulatory, or clinical purpose.

Band SNR requires an estimate of the noise present in a signal. As with
noise_suppression.py, this is accurate when a noise-only reference clip is
supplied, and an estimate (from the quietest frames) otherwise.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import signal as sps

# 18 one-third-octave-ish bands spanning the speech-relevant 150-8000 Hz
# range, with approximate "average speech" importance weights loosely
# following the shape of the ANSI S3.5 band-importance function. These
# weights are illustrative, not transcribed from the standard, and are
# re-normalized to sum to 1.0 below. Verify against ANSI S3.5-1997 Table 3
# before using this for anything beyond relative before/after comparison.
_BAND_CENTERS_HZ = [
    160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250,
    1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
]
_RAW_IMPORTANCE = [
    0.0083, 0.0095, 0.0150, 0.0289, 0.0440, 0.0578, 0.0653, 0.0711, 0.0818, 0.0844,
    0.0882, 0.0898, 0.0868, 0.0844, 0.0771, 0.0527, 0.0364, 0.0285,
]
_IMPORTANCE = np.array(_RAW_IMPORTANCE) / np.sum(_RAW_IMPORTANCE)

_EPS = 1e-12


def _band_edges_hz() -> list[tuple[float, float]]:
    centers = np.array(_BAND_CENTERS_HZ, dtype=float)
    edges = []
    for i, center in enumerate(centers):
        lower = np.sqrt(centers[i - 1] * center) if i > 0 else center / (2 ** (1 / 6))
        upper = np.sqrt(centers[i + 1] * center) if i < len(centers) - 1 else center * (2 ** (1 / 6))
        edges.append((lower, upper))
    return edges


def _band_power(signal_data: np.ndarray, sample_rate_hz: int, low_hz: float, high_hz: float) -> float:
    nyquist = sample_rate_hz / 2.0
    low = max(1.0, low_hz) / nyquist
    high = min(high_hz, nyquist * 0.999) / nyquist
    if low >= high:
        return 0.0
    sos = sps.butter(4, [low, high], btype="bandpass", output="sos")
    filtered = sps.sosfilt(sos, signal_data)
    return float(np.mean(np.square(filtered)))


def _band_audibility(snr_db: float) -> float:
    """Map per-band SNR to an audibility fraction in [0, 1].

    Uses a 30 dB linear audibility range centered on 0 dB SNR (i.e. -15 dB to
    +15 dB), a common simplification of the SII transfer function. Values
    outside the range saturate at 0 or 1.
    """
    return float(np.clip((snr_db + 15.0) / 30.0, 0.0, 1.0))


@dataclass
class SiiResult:
    method: str  # "reference" or "estimated"
    value: float  # in [0, 1]
    band_snr_db: list[float]

    def as_dict(self) -> dict:
        return {"method": self.method, "value": self.value, "band_snr_db": self.band_snr_db}


def compute_sii(
    signal_data: np.ndarray,
    sample_rate_hz: int,
    noise_reference: np.ndarray | None = None,
) -> SiiResult:
    """Compute the approximate SII for one signal.

    If ``noise_reference`` (a noise-only clip, same domain as ``signal_data``)
    is provided, per-band SNR uses that reference directly. Otherwise, the
    noise floor is estimated per band from the signal's own quietest 20 ms
    frames (see noise_suppression.estimate_noise_floor_dbfs for the same
    approach applied broadband).
    """
    edges = _band_edges_hz()
    band_snr_db: list[float] = []
    method = "reference" if noise_reference is not None else "estimated"

    for low_hz, high_hz in edges:
        signal_power = _band_power(signal_data, sample_rate_hz, low_hz, high_hz)
        if noise_reference is not None:
            noise_power = _band_power(noise_reference, sample_rate_hz, low_hz, high_hz)
        else:
            frame_len = max(1, int(sample_rate_hz * 0.02))
            n_frames = max(1, len(signal_data) // frame_len)
            trimmed = signal_data[: n_frames * frame_len].reshape(n_frames, frame_len)
            frame_powers = [
                _band_power(frame, sample_rate_hz, low_hz, high_hz) for frame in trimmed
            ]
            noise_power = float(np.percentile(frame_powers, 10.0)) if frame_powers else _EPS
        snr_db = 10.0 * np.log10((signal_power + _EPS) / (noise_power + _EPS))
        band_snr_db.append(snr_db)

    audibility = np.array([_band_audibility(snr) for snr in band_snr_db])
    sii_value = float(np.sum(_IMPORTANCE * audibility))
    return SiiResult(method=method, value=sii_value, band_snr_db=band_snr_db)


@dataclass
class SiiComparison:
    sii_before: SiiResult
    sii_after: SiiResult
    improvement: float

    def as_dict(self) -> dict:
        return {
            "sii_before": self.sii_before.as_dict(),
            "sii_after": self.sii_after.as_dict(),
            "sii_improvement": self.improvement,
        }


def compute_sii_before_after(
    before: np.ndarray,
    after: np.ndarray,
    sample_rate_hz: int,
    *,
    noise_reference_before: np.ndarray | None = None,
    noise_reference_after: np.ndarray | None = None,
) -> SiiComparison:
    sii_before = compute_sii(before, sample_rate_hz, noise_reference_before)
    sii_after = compute_sii(after, sample_rate_hz, noise_reference_after)
    return SiiComparison(
        sii_before=sii_before,
        sii_after=sii_after,
        improvement=sii_after.value - sii_before.value,
    )
