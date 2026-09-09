from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass

import numpy as np
from scipy.signal import lfilter, resample_poly

try:
    import soxr  # type: ignore
except Exception:
    soxr = None


def dbfs_to_linear(db: float) -> float:
    return float(10.0 ** (float(db) / 20.0))


def render_monitor(levels: np.ndarray, statuses: list[str]) -> str:
    split = max(1, len(levels) // 2)
    line1: list[str] = []
    line2: list[str] = []
    for ch in range(len(levels)):
        token = f"ch{ch}: {levels[ch]:6.1f} dBFS {statuses[ch]:7s}"
        if ch < split:
            line1.append(token)
        else:
            line2.append(token)
    if line2:
        return "    ".join(line1) + "\n" + "    ".join(line2)
    return "    ".join(line1)


def resample_poly_exact(x: np.ndarray, src_sr: int, dst_sr: int, axis: int = -1) -> np.ndarray:
    x = np.asarray(x)
    if src_sr <= 0 or dst_sr <= 0:
        raise ValueError("Sample rates must be positive.")
    if x.shape[axis] == 0 or src_sr == dst_sr:
        return x.astype(np.float32, copy=False)

    g = math.gcd(int(src_sr), int(dst_sr))
    up = int(dst_sr) // g
    down = int(src_sr) // g
    y = resample_poly(x, up=up, down=down, axis=axis, window=("kaiser", 8.6))

    in_len = x.shape[axis]
    out_len = int(round(in_len * dst_sr / src_sr))
    if y.shape[axis] > out_len:
        slicer = [slice(None)] * y.ndim
        slicer[axis] = slice(0, out_len)
        y = y[tuple(slicer)]
    elif y.shape[axis] < out_len:
        pad_width = [(0, 0)] * y.ndim
        pad_width[axis] = (0, out_len - y.shape[axis])
        y = np.pad(y, pad_width, mode="constant")
    return y.astype(np.float32, copy=False)


class StreamingResampler:
    """
    Stateful streaming SRC wrapper.

    Primary backend is SoXR (streaming state kept internally). If SoXR is not
    available, this falls back to a bounded-history polyphase approach that
    preserves filter history across blocks.
    """

    def __init__(self, *, src_sr: int, dst_sr: int, num_channels: int, quality: str = "VHQ") -> None:
        self.src_sr = int(src_sr)
        self.dst_sr = int(dst_sr)
        self.num_channels = int(num_channels)
        self.quality = str(quality)

        if self.src_sr <= 0 or self.dst_sr <= 0:
            raise ValueError("Sample rates must be positive.")
        if self.num_channels <= 0:
            raise ValueError("num_channels must be positive.")

        self._use_soxr = soxr is not None and hasattr(soxr, "ResampleStream")
        self.backend = "soxr-stream" if self._use_soxr else "polyphase-fallback"
        self._stream = None

        # Polyphase streaming fallback state.
        g = math.gcd(self.src_sr, self.dst_sr)
        self._up = self.dst_sr // g
        self._down = self.src_sr // g
        # Conservative history to preserve FIR phase across blocks.
        self._hist_len = int(max(32, 16 * max(self._up, self._down)))
        self._hist = np.zeros((self.num_channels, self._hist_len), dtype=np.float32)

        if self._use_soxr:
            # soxr.ResampleStream accepts interleaved [T,C] float32.
            self._stream = soxr.ResampleStream(self.src_sr, self.dst_sr, self.num_channels, dtype="float32", quality=self.quality)

    def reset(self) -> None:
        if self._use_soxr:
            self._stream = soxr.ResampleStream(self.src_sr, self.dst_sr, self.num_channels, dtype="float32", quality=self.quality)
        self._hist.fill(0.0)

    def process(self, x: np.ndarray) -> np.ndarray:
        x = np.asarray(x, dtype=np.float32)
        if x.ndim != 2 or x.shape[0] != self.num_channels:
            raise ValueError(f"expected [C,T] with C={self.num_channels}, got {x.shape}")
        if x.shape[1] == 0 or self.src_sr == self.dst_sr:
            return x.astype(np.float32, copy=False)

        if self._use_soxr and self._stream is not None:
            # soxr expects [T,C]
            y_tc = self._stream.resample_chunk(x.T, last=False)
            return np.asarray(y_tc, dtype=np.float32).T

        # Polyphase fallback: prepend history and discard its resampled contribution.
        x_ext = np.concatenate([self._hist, x], axis=1)
        y_ext = resample_poly(x_ext, up=self._up, down=self._down, axis=-1, window=("kaiser", 8.6)).astype(np.float32, copy=False)

        in_len = int(x_ext.shape[-1])
        out_len = int(round(in_len * self.dst_sr / self.src_sr))
        if y_ext.shape[-1] > out_len:
            y_ext = y_ext[..., :out_len]
        elif y_ext.shape[-1] < out_len:
            y_ext = np.pad(y_ext, ((0, 0), (0, out_len - y_ext.shape[-1])), mode="constant")

        hist_out_len = int(round(self._hist_len * self.dst_sr / self.src_sr))
        y = y_ext[:, hist_out_len:]

        # Update history with newest samples.
        if x.shape[-1] >= self._hist_len:
            self._hist[:, :] = x[:, -self._hist_len :]
        else:
            self._hist = np.concatenate([self._hist[:, x.shape[-1] :], x], axis=1)
        return y.astype(np.float32, copy=False)


class DCOffsetFilter:
    def __init__(self, num_channels: int, alpha: float) -> None:
        self.num_channels = int(num_channels)
        self.alpha = float(alpha)
        self.prev_x = np.zeros(self.num_channels, dtype=np.float32)
        self.prev_y = np.zeros(self.num_channels, dtype=np.float32)

        # Difference equation:
        #   y[n] = x[n] - x[n-1] + alpha * y[n-1]
        # Implemented via scipy.signal.lfilter with per-channel state.
        self._b = np.array([1.0, -1.0], dtype=np.float32)
        self._a = np.array([1.0, -self.alpha], dtype=np.float32)

    def process(self, x: np.ndarray) -> np.ndarray:
        # x: [C, T]
        x = np.asarray(x, dtype=np.float32)
        if x.ndim != 2 or x.shape[0] != self.num_channels:
            raise ValueError(f"expected [C,T] with C={self.num_channels}, got {x.shape}")
        if x.shape[1] == 0:
            return x.astype(np.float32, copy=False)

        # For this 2-tap IIR, lfilter's initial condition maps as:
        #   y[0] = b0*x[0] + zi0, with b0=1
        # We want y[0] = x[0] - prev_x + alpha*prev_y, so zi0 = -prev_x + alpha*prev_y.
        zi0 = (-self.prev_x + self.alpha * self.prev_y).astype(np.float32, copy=False)[:, None]
        y, zf = lfilter(self._b, self._a, x, axis=-1, zi=zi0)
        # Maintain prior semantics for other code that inspects these.
        self.prev_x[:] = x[:, -1]
        self.prev_y[:] = y[:, -1]
        return y.astype(np.float32, copy=False)


class ChannelLevelValidator:
    def __init__(
        self,
        num_channels: int,
        silence_dbfs: float,
        fault_min_dbfs: float,
        fault_max_dbfs: float,
        silence_window_frames: int,
    ) -> None:
        self.num_channels = int(num_channels)
        self.silence_dbfs = float(silence_dbfs)
        self.fault_min_dbfs = float(fault_min_dbfs)
        self.fault_max_dbfs = float(fault_max_dbfs)
        self.silence_window_frames = max(1, int(silence_window_frames))
        self.silence_counts = np.zeros(self.num_channels, dtype=np.int64)
        self.fault_counts = np.zeros(self.num_channels, dtype=np.int64)
        self.power_history: deque[np.ndarray] = deque(maxlen=self.silence_window_frames)
        self.power_sum = np.zeros(self.num_channels, dtype=np.float64)

    @staticmethod
    def _rms_dbfs(channel: np.ndarray) -> float:
        rms = float(np.sqrt(np.mean(np.square(channel), dtype=np.float64) + 1e-12))
        return float(20.0 * np.log10(max(rms, 1e-10)))

    def validate(self, x: np.ndarray, frame_idx: int) -> tuple[np.ndarray, np.ndarray, list[str]]:
        y = x.copy()
        frame_levels = np.zeros(self.num_channels, dtype=np.float32)
        statuses: list[str] = ["ok"] * self.num_channels
        frame_power = np.mean(np.square(y, dtype=np.float64), axis=1).astype(np.float32)
        if len(self.power_history) == self.power_history.maxlen:
            oldest = self.power_history.popleft()
            self.power_sum -= oldest.astype(np.float64)
        self.power_history.append(frame_power)
        self.power_sum += frame_power.astype(np.float64)
        rolling_power = (self.power_sum / max(1, len(self.power_history))).astype(np.float32)
        rolling_levels = np.array(
            [float(20.0 * np.log10(max(float(np.sqrt(p + 1e-12)), 1e-10))) for p in rolling_power],
            dtype=np.float32,
        )
        for ch in range(self.num_channels):
            dbfs = self._rms_dbfs(y[ch])
            frame_levels[ch] = dbfs
            if rolling_levels[ch] < self.silence_dbfs:
                y[ch, :] = 0.0
                statuses[ch] = "silence"
                self.silence_counts[ch] += 1
            elif self.fault_min_dbfs <= dbfs <= self.fault_max_dbfs:
                y[ch, :] = 0.0
                statuses[ch] = "FAULT"
                self.fault_counts[ch] += 1
            else:
                statuses[ch] = "ok"
        return y, frame_levels, statuses


class DriftMonitor:
    def __init__(
        self,
        block_duration_sec: float,
        target_fill: int,
        window_sec: float,
        update_every: int,
        max_ppm: float,
    ) -> None:
        import time

        self._time = time
        self.block_duration_sec = float(block_duration_sec)
        self.target_fill = max(1, int(target_fill))
        self.window_sec = float(window_sec)
        self.update_every = max(1, int(update_every))
        self.max_ppm = float(max_ppm)
        self.samples: deque[tuple[float, float]] = deque()
        self.counter = 0
        self.current_ppm = 0.0
        self.current_drift_ppm = 0.0

    def update(self, fill_level: int) -> tuple[float, float]:
        self.counter += 1
        now = self._time.perf_counter()
        self.samples.append((now, float(fill_level)))
        while len(self.samples) > 2 and (now - self.samples[0][0]) > self.window_sec:
            self.samples.popleft()

        if self.counter % self.update_every != 0:
            return self.current_ppm, (1.0 - self.current_ppm * 1e-6)

        if len(self.samples) >= 2:
            t0, f0 = self.samples[0]
            t1, f1 = self.samples[-1]
            dt = max(1e-6, t1 - t0)
            slope_blocks_per_sec = (f1 - f0) / dt
            nominal_blocks_per_sec = 1.0 / max(1e-6, self.block_duration_sec)
            self.current_drift_ppm = float((slope_blocks_per_sec / nominal_blocks_per_sec) * 1e6)
        else:
            self.current_drift_ppm = 0.0

        error_blocks = float(fill_level - self.target_fill)
        error_ppm = (error_blocks / self.target_fill) * (self.block_duration_sec / self.window_sec) * 1e6
        desired_ppm = self.current_drift_ppm + error_ppm
        desired_ppm = float(np.clip(desired_ppm, -self.max_ppm, self.max_ppm))
        self.current_ppm = 0.9 * self.current_ppm + 0.1 * desired_ppm
        self.current_ppm = float(np.clip(self.current_ppm, -self.max_ppm, self.max_ppm))
        return self.current_ppm, (1.0 - self.current_ppm * 1e-6)


@dataclass
class ProfileStats:
    capture_to_frame_ms: float = 0.0
    dc_ms: float = 0.0
    level_ms: float = 0.0
    frame_to_infer_ms: float = 0.0
    infer_ms: float = 0.0
    post_ms: float = 0.0
    e2e_ms: float = 0.0
    drift_ppm: float = 0.0

    def update(self, other: "ProfileStats", alpha: float = 0.1) -> None:
        self.capture_to_frame_ms = (1 - alpha) * self.capture_to_frame_ms + alpha * other.capture_to_frame_ms
        self.dc_ms = (1 - alpha) * self.dc_ms + alpha * other.dc_ms
        self.level_ms = (1 - alpha) * self.level_ms + alpha * other.level_ms
        self.frame_to_infer_ms = (1 - alpha) * self.frame_to_infer_ms + alpha * other.frame_to_infer_ms
        self.infer_ms = (1 - alpha) * self.infer_ms + alpha * other.infer_ms
        self.post_ms = (1 - alpha) * self.post_ms + alpha * other.post_ms
        self.e2e_ms = (1 - alpha) * self.e2e_ms + alpha * other.e2e_ms
        self.drift_ppm = (1 - alpha) * self.drift_ppm + alpha * other.drift_ppm

