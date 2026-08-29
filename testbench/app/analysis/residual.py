"""Residual/error signal computation.

IMPORTANT: the beamformer changes channel count (6 -> 1) and applies a
steering-vector delay plus its own 128/32 STFT (127 samples). A raw sample-domain
subtraction between the 6-channel input and the mono/stereo output is not
meaningful (see testbench/README.md, "Residual definition"). Only same-domain,
same-alignment signal pairs should ever be subtracted here:

  - beamform residual = delay-aligned pre-suppression mono − post-suppression mono
  - limiter residual   = pre-limiter mono      - post-limiter mono

Both operands come from sonitude_wav_replay's diagnostic taps
(--output-beamformed / --output-suppressed) and its final --output.
The limiter pair is already time-aligned. The beamform pair is not when the
spectral backend is active: the suppressed tap includes
suppression_algorithmic_delay_samples (127 at FFT 128). Callers must pass
that delay so the undelayed beamformed reference is shifted before subtraction.
"""

from __future__ import annotations

import numpy as np


class IncompatibleSignalsError(ValueError):
    """Raised when two signals cannot be meaningfully subtracted."""


def trim_to_shortest(a: np.ndarray, b: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    n = min(len(a), len(b))
    return a[:n], b[:n]


def delay_align(pre_stage: np.ndarray, post_stage: np.ndarray, delay_samples: int) -> tuple[np.ndarray, np.ndarray]:
    """Align an undelayed pre tap to a delayed post tap.

    ``post_stage[n]`` corresponds to ``pre_stage[n - delay_samples]`` when the
    stage is a causal STFT of delay ``delay_samples``.
    """
    pre_stage = np.asarray(pre_stage)
    post_stage = np.asarray(post_stage)
    delay = max(0, int(delay_samples))
    pre_stage, post_stage = trim_to_shortest(pre_stage, post_stage)
    if delay == 0:
        return pre_stage, post_stage
    if delay >= len(pre_stage):
        empty = pre_stage[:0]
        return empty, empty
    return pre_stage[: len(pre_stage) - delay], post_stage[delay:]


def compute_stage_residual(
    pre_stage: np.ndarray, post_stage: np.ndarray, *, delay_samples: int = 0
) -> np.ndarray:
    """Subtract two same-domain, same-channel-count mono/stereo signals.

    Raises IncompatibleSignalsError if channel counts differ, since that
    signals a caller is about to do an invalid raw-vs-processed subtraction.
    ``delay_samples`` is the extra delay in ``post_stage`` relative to ``pre_stage``.
    """
    pre_stage = np.asarray(pre_stage)
    post_stage = np.asarray(post_stage)
    pre_channels = pre_stage.shape[1] if pre_stage.ndim > 1 else 1
    post_channels = post_stage.shape[1] if post_stage.ndim > 1 else 1
    if pre_channels != post_channels:
        raise IncompatibleSignalsError(
            f"Cannot subtract signals with different channel counts ({pre_channels} vs "
            f"{post_channels}); beamforming changes channel count and alignment, so raw "
            "input cannot be subtracted from processed output directly."
        )
    pre_stage, post_stage = delay_align(pre_stage, post_stage, delay_samples)
    return pre_stage - post_stage


def residual_energy_ratio_db(
    pre_stage: np.ndarray, post_stage: np.ndarray, *, delay_samples: int = 0
) -> float:
    """How much energy the stage removed/added, in dB (10*log10(residual/pre))."""
    pre_stage, post_stage = delay_align(pre_stage, post_stage, delay_samples)
    residual = compute_stage_residual(pre_stage, post_stage, delay_samples=0)
    pre_energy = float(np.mean(np.square(pre_stage[: len(residual)])))
    residual_energy = float(np.mean(np.square(residual)))
    if pre_energy <= 0.0:
        return float("-inf")
    if residual_energy <= 0.0:
        return float("-inf")
    return 10.0 * np.log10(residual_energy / pre_energy)
