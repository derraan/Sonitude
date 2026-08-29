"""Residual/error signal computation.

IMPORTANT: the beamformer changes channel count (6 -> 1) and applies a
per-channel steering delay, so a raw sample-domain subtraction between the
6-channel input and the mono/stereo output is not meaningful (see
testbench/README.md, "Residual definition"). Only same-domain, same-alignment
signal pairs should ever be subtracted here:

  - beamform residual = pre-suppression mono  - post-suppression mono
  - limiter residual   = pre-limiter mono      - post-limiter mono

Both operands come from sonitude_wav_replay's diagnostic taps
(--output-beamformed / --output-suppressed) and its final --output, so they
are already time-aligned and same-length by construction.
"""

from __future__ import annotations

import numpy as np


class IncompatibleSignalsError(ValueError):
    """Raised when two signals cannot be meaningfully subtracted."""


def trim_to_shortest(a: np.ndarray, b: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    n = min(len(a), len(b))
    return a[:n], b[:n]


def compute_stage_residual(pre_stage: np.ndarray, post_stage: np.ndarray) -> np.ndarray:
    """Subtract two same-domain, same-channel-count mono/stereo signals.

    Raises IncompatibleSignalsError if channel counts differ, since that
    signals a caller is about to do an invalid raw-vs-processed subtraction.
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
    pre_stage, post_stage = trim_to_shortest(pre_stage, post_stage)
    return pre_stage - post_stage


def residual_energy_ratio_db(pre_stage: np.ndarray, post_stage: np.ndarray) -> float:
    """How much energy the stage removed/added, in dB (10*log10(residual/pre))."""
    residual = compute_stage_residual(pre_stage, post_stage)
    pre_energy = float(np.mean(np.square(pre_stage[: len(residual)])))
    residual_energy = float(np.mean(np.square(residual)))
    if pre_energy <= 0.0:
        return float("-inf")
    if residual_energy <= 0.0:
        return float("-inf")
    return 10.0 * np.log10(residual_energy / pre_energy)
