"""Noise-suppression related level metrics.

These ratios are mixture-power versus estimated or reference noise-power.
They are NOT a strict speech-SNR measurement (no VAD, no speech-only
numerator). Keys keep historical ``snr_*`` names as compatibility aliases.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

_FRAME_MS = 20.0
_EPS = 1e-12


def _frame_rms(signal: np.ndarray, sample_rate_hz: int, frame_ms: float = _FRAME_MS) -> np.ndarray:
    frame_len = max(1, int(sample_rate_hz * frame_ms / 1000.0))
    n_frames = max(1, len(signal) // frame_len)
    trimmed = signal[: n_frames * frame_len]
    frames = trimmed.reshape(n_frames, frame_len)
    return np.sqrt(np.mean(np.square(frames), axis=1) + _EPS)


def rms_dbfs(signal: np.ndarray) -> float:
    rms = float(np.sqrt(np.mean(np.square(signal)) + _EPS))
    return 20.0 * np.log10(rms + _EPS)


def estimate_noise_floor_dbfs(signal: np.ndarray, sample_rate_hz: int, percentile: float = 10.0) -> float:
    """Estimate the noise floor from the quietest frames of a signal."""
    frame_rms = _frame_rms(signal, sample_rate_hz)
    noise_rms = float(np.percentile(frame_rms, percentile))
    return 20.0 * np.log10(noise_rms + _EPS)


def estimate_mixture_to_noise_db(signal: np.ndarray, sample_rate_hz: int, noise_percentile: float = 10.0) -> float:
    """Mixture RMS vs. estimated noise-floor RMS, in dB. Not speech SNR."""
    signal_level = rms_dbfs(signal)
    noise_level = estimate_noise_floor_dbfs(signal, sample_rate_hz, noise_percentile)
    return signal_level - noise_level


estimate_snr_db = estimate_mixture_to_noise_db


def reference_mixture_to_noise_db(signal: np.ndarray, noise_reference: np.ndarray) -> float:
    """Mixture power vs. a noise-only reference clip, in dB. Not speech SNR."""
    signal_power = float(np.mean(np.square(signal)))
    noise_power = float(np.mean(np.square(noise_reference)))
    if noise_power <= 0.0:
        return float("inf")
    return 10.0 * np.log10((signal_power + _EPS) / (noise_power + _EPS))


reference_snr_db = reference_mixture_to_noise_db


@dataclass
class NoiseSuppressionMetrics:
    method: str  # "reference" or "estimated"
    input_noise_floor_dbfs: float
    output_noise_floor_dbfs: float
    noise_reduction_db: float
    snr_before_db: float
    snr_after_db: float
    snr_improvement_db: float

    def as_dict(self) -> dict:
        return {
            "method": self.method,
            "definition": "mixture_power_over_noise_power_db; not speech SNR",
            "input_noise_floor_dbfs": self.input_noise_floor_dbfs,
            "output_noise_floor_dbfs": self.output_noise_floor_dbfs,
            "noise_reduction_db": self.noise_reduction_db,
            "mixture_to_noise_before_db": self.snr_before_db,
            "mixture_to_noise_after_db": self.snr_after_db,
            "mixture_to_noise_improvement_db": self.snr_improvement_db,
            "snr_before_db": self.snr_before_db,
            "snr_after_db": self.snr_after_db,
            "snr_improvement_db": self.snr_improvement_db,
        }


def compute_noise_suppression_metrics(
    before: np.ndarray,
    after: np.ndarray,
    sample_rate_hz: int,
    *,
    noise_reference_before: np.ndarray | None = None,
    noise_reference_after: np.ndarray | None = None,
) -> NoiseSuppressionMetrics:
    """Compute before/after noise metrics.

    Uses reference-based SNR when noise-only clips are supplied for both
    signals; otherwise falls back to the percentile noise-floor estimate and
    labels the result ``"estimated"``.
    """
    if noise_reference_before is not None and noise_reference_after is not None:
        method = "reference"
        input_noise_floor = rms_dbfs(noise_reference_before)
        output_noise_floor = rms_dbfs(noise_reference_after)
        snr_before = reference_mixture_to_noise_db(before, noise_reference_before)
        snr_after = reference_mixture_to_noise_db(after, noise_reference_after)
    else:
        method = "estimated"
        input_noise_floor = estimate_noise_floor_dbfs(before, sample_rate_hz)
        output_noise_floor = estimate_noise_floor_dbfs(after, sample_rate_hz)
        snr_before = estimate_snr_db(before, sample_rate_hz)
        snr_after = estimate_snr_db(after, sample_rate_hz)

    return NoiseSuppressionMetrics(
        method=method,
        input_noise_floor_dbfs=input_noise_floor,
        output_noise_floor_dbfs=output_noise_floor,
        noise_reduction_db=input_noise_floor - output_noise_floor,
        snr_before_db=snr_before,
        snr_after_db=snr_after,
        snr_improvement_db=snr_after - snr_before,
    )
