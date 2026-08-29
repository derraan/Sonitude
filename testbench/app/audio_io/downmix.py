"""Listening-preview downmixes. Not part of the algorithm — playback only.

For "before" listening, the pipeline itself has no defined stereo downmix of
the 6-channel input (that's exactly what the beamformer is for). To let the
GUI play something for RAW/A-B comparison, this mirrors the one convention
that already exists in the codebase: main.cpp's ``--mode passthrough`` taps
ear-cup mics (channels 4 and 5 of the active channel map) straight to L/R.
This is a playback convenience only and must never feed the algorithm or a
residual computation.
"""

from __future__ import annotations

import numpy as np

EAR_CUP_LEFT_INDEX = 4
EAR_CUP_RIGHT_INDEX = 5


def ear_cup_stereo_preview(six_channel_data: np.ndarray, active_channel_map: list[int]) -> np.ndarray:
    """Select the ear-cup mic pair as an uncalibrated stereo preview.

    ``six_channel_data`` must already be the 6 active mic channels in
    active_channel_map order, shape (num_samples, 6).
    """
    if six_channel_data.shape[1] < 6:
        raise ValueError("expected at least 6 channels for ear-cup preview")
    left = six_channel_data[:, EAR_CUP_LEFT_INDEX]
    right = six_channel_data[:, EAR_CUP_RIGHT_INDEX]
    return np.stack([left, right], axis=1)


def mono_to_stereo(mono: np.ndarray) -> np.ndarray:
    """Duplicate a mono signal to stereo, matching the algorithm's own
    mono-to-stereo convention (no HRTF in v1, see docs/architecture.md)."""
    mono = np.asarray(mono).reshape(-1)
    return np.stack([mono, mono], axis=1)
