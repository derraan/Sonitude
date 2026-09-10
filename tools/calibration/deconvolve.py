"""Sweep deconvolution and direct-path window helpers for measured MVDR."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class DirectPathWindowSpec:
    pre_samples: int = 8
    length_samples: int = 256
    taper: str = "tukey"
    tukey_alpha: float = 0.25


@dataclass(frozen=True)
class DirectPathWindowResult:
    windowed: np.ndarray  # [mics, time]
    start_sample: int
    stop_sample: int
    arrival_samples: list[int]


def _next_pow2(n: int) -> int:
    out = 1
    while out < n:
        out <<= 1
    return out


def _tukey(length: int, alpha: float) -> np.ndarray:
    if length <= 0:
        return np.zeros(0, dtype=np.float64)
    if alpha <= 0.0:
        return np.ones(length, dtype=np.float64)
    if alpha >= 1.0:
        return np.hanning(length).astype(np.float64)
    w = np.ones(length, dtype=np.float64)
    edge = int(np.floor(alpha * (length - 1) / 2.0))
    if edge <= 0:
        return w
    n = np.arange(edge, dtype=np.float64)
    w[:edge] = 0.5 * (1.0 + np.cos(np.pi * ((2.0 * n / (alpha * (length - 1))) - 1.0)))
    w[-edge:] = w[:edge][::-1]
    return w


def regularized_deconvolution(
    recording: np.ndarray, stimulus: np.ndarray, *, eps_rel: float = 1.0e-6
) -> np.ndarray:
    """Recover an impulse response from sweep recording and known stimulus.

    recording: 1-D capture at one microphone.
    stimulus: 1-D played sweep.
    """

    rec = np.asarray(recording, dtype=np.float64).reshape(-1)
    stim = np.asarray(stimulus, dtype=np.float64).reshape(-1)
    if rec.size == 0 or stim.size == 0:
        raise ValueError("recording and stimulus must be non-empty")
    n_fft = _next_pow2(rec.size + stim.size - 1)
    rec_fft = np.fft.rfft(rec, n=n_fft)
    stim_fft = np.fft.rfft(stim, n=n_fft)
    denom = np.abs(stim_fft) ** 2
    eps = max(float(np.max(denom)) * float(eps_rel), 1.0e-18)
    ir = np.fft.irfft(rec_fft * np.conj(stim_fft) / (denom + eps), n=n_fft)
    return ir[: rec.size].copy()


def deconvolve_multichannel(
    recording: np.ndarray, stimulus: np.ndarray, *, eps_rel: float = 1.0e-6
) -> np.ndarray:
    """Deconvolve a multichannel sweep recording into per-mic IRs.

    recording shape must be [time, mics] and output is [mics, time].
    """

    rec = np.asarray(recording, dtype=np.float64)
    if rec.ndim != 2:
        raise ValueError("recording must be [time, channels]")
    out = np.zeros((rec.shape[1], rec.shape[0]), dtype=np.float64)
    for ch in range(rec.shape[1]):
        out[ch] = regularized_deconvolution(rec[:, ch], stimulus, eps_rel=eps_rel)
    return out


def direct_path_window(irs: np.ndarray, spec: DirectPathWindowSpec) -> DirectPathWindowResult:
    """Apply one shared direct-path window across all microphones.

    The shared origin preserves inter-microphone phase differences.
    """

    cube = np.asarray(irs, dtype=np.float64)
    if cube.ndim != 2:
        raise ValueError("irs must be [mics, time]")
    if spec.length_samples <= 0:
        raise ValueError("window length_samples must be > 0")

    arrival = [int(np.argmax(np.abs(cube[ch]))) for ch in range(cube.shape[0])]
    anchor = min(arrival)
    start = max(0, anchor - int(spec.pre_samples))
    stop = min(cube.shape[1], start + int(spec.length_samples))
    start = max(0, stop - int(spec.length_samples))
    length = stop - start

    if spec.taper == "rect":
        win = np.ones(length, dtype=np.float64)
    elif spec.taper == "hann":
        win = np.hanning(length).astype(np.float64)
    elif spec.taper == "tukey":
        win = _tukey(length, float(spec.tukey_alpha))
    else:
        raise ValueError(f"unsupported taper: {spec.taper}")

    out = np.zeros_like(cube)
    out[:, start:stop] = cube[:, start:stop] * win[None, :]
    return DirectPathWindowResult(
        windowed=out,
        start_sample=start,
        stop_sample=stop,
        arrival_samples=arrival,
    )
