from __future__ import annotations

import numpy as np


def interpolate_filters(h_prev: np.ndarray, h_cur: np.ndarray, hop_size: int) -> np.ndarray:
    """
    Returns per-sample interpolated filters for one hop.
    h_prev/h_cur shape: [num_mics, filter_len]
    output shape: [hop_size, num_mics, filter_len]
    """
    out = np.empty((hop_size, h_prev.shape[0], h_prev.shape[1]), dtype=np.float32)
    for i in range(hop_size):
        t = float(i + 1) / float(hop_size)
        out[i] = h_prev + (h_cur - h_prev) * t
    return out


def filter_and_sum_with_interpolation(
    frame_by_mic: np.ndarray, h_prev: np.ndarray, h_cur: np.ndarray, hop_size: int
) -> np.ndarray:
    """
    Implements paper eq. (7)-(8) for one hop.
    frame_by_mic shape: [num_mics, hop_size + Lp + Lf]
    h_prev/h_cur shape: [num_mics, filter_len], filter_len = 1 + Lp + Lf
    returns: [hop_size]
    """
    num_mics, frame_len = frame_by_mic.shape
    filter_len = h_cur.shape[1]
    if frame_len < hop_size + filter_len - 1:
        raise ValueError("frame_by_mic too short for hop/filter sizes")
    h_interp = interpolate_filters(h_prev, h_cur, hop_size)
    out = np.zeros((hop_size,), dtype=np.float32)
    for i in range(hop_size):
        yi = frame_by_mic[:, i : i + filter_len]
        out[i] = np.sum(h_interp[i] * yi)
    return out


def naive_filter_and_sum(frame_by_mic: np.ndarray, h_per_sample: np.ndarray) -> np.ndarray:
    """
    Direct per-sample implementation.
    frame_by_mic: [num_mics, hop_size + filter_len - 1]
    h_per_sample: [hop_size, num_mics, filter_len]
    """
    hop_size = h_per_sample.shape[0]
    filter_len = h_per_sample.shape[2]
    out = np.zeros((hop_size,), dtype=np.float32)
    for i in range(hop_size):
        for m in range(frame_by_mic.shape[0]):
            for k in range(filter_len):
                out[i] += h_per_sample[i, m, k] * frame_by_mic[m, i + k]
    return out
