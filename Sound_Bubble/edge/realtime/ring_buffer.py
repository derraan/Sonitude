from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass
class RingStats:
    dropped_samples: int = 0


class AudioRingBuffer:
    """
    Fixed-capacity ring buffer for audio samples.

    Shape is [C, T]. Push appends samples; if overflow occurs, drops oldest
    samples (keeps newest). Pop consumes exactly n samples if available.
    """

    def __init__(self, num_channels: int, capacity_samples: int) -> None:
        self.num_channels = int(num_channels)
        self.capacity = max(1, int(capacity_samples))
        self._buf = np.zeros((self.num_channels, self.capacity), dtype=np.float32)
        self._write = 0
        self._size = 0
        self.stats = RingStats()

    def size(self) -> int:
        return int(self._size)

    def push(self, x: np.ndarray) -> None:
        x = np.asarray(x, dtype=np.float32)
        if x.ndim != 2 or x.shape[0] != self.num_channels:
            raise ValueError(f"expected [C,T] with C={self.num_channels}, got {x.shape}")
        n = int(x.shape[1])
        if n <= 0:
            return

        if n >= self.capacity:
            # Keep only the newest capacity samples.
            self._buf[:, :] = x[:, -self.capacity :]
            self._write = 0
            dropped = self._size + (n - self.capacity)
            self._size = self.capacity
            self.stats.dropped_samples += int(max(0, dropped))
            return

        overflow = max(0, (self._size + n) - self.capacity)
        if overflow:
            self._size -= overflow
            self.stats.dropped_samples += int(overflow)

        # Write may wrap.
        w = self._write
        end = w + n
        if end <= self.capacity:
            self._buf[:, w:end] = x
        else:
            n1 = self.capacity - w
            self._buf[:, w:] = x[:, :n1]
            self._buf[:, : end % self.capacity] = x[:, n1:]
        self._write = end % self.capacity
        self._size = min(self.capacity, self._size + n)

    def pop(self, n: int) -> np.ndarray | None:
        n = int(n)
        if n <= 0:
            raise ValueError("n must be > 0")
        if self._size < n:
            return None

        # Oldest sample index is write - size.
        start = (self._write - self._size) % self.capacity
        end = start + n
        if end <= self.capacity:
            out = self._buf[:, start:end].copy()
        else:
            n1 = self.capacity - start
            out = np.concatenate([self._buf[:, start:].copy(), self._buf[:, : end % self.capacity].copy()], axis=1)
        self._size -= n
        return out


@dataclass
class OutputRingStats:
    dropped_oldest_frames: int = 0
    underrun_frames: int = 0
    write_calls: int = 0
    read_calls: int = 0


class OutputRingBuffer:
    def __init__(self, num_channels: int, capacity_frames: int) -> None:
        import threading

        self.num_channels = int(num_channels)
        self.capacity = max(1, int(capacity_frames))
        self._buf = np.zeros((self.capacity, self.num_channels), dtype=np.float32)
        self._read = 0
        self._write = 0
        self._size = 0
        self._lock = threading.Lock()
        self.stats = OutputRingStats()

    def capacity_frames(self) -> int:
        return int(self.capacity)

    def ahead_frames(self) -> int:
        with self._lock:
            return int(self._size)

    def clear(self) -> None:
        with self._lock:
            self._read = 0
            self._write = 0
            self._size = 0

    def drop_oldest_frames(self, n: int) -> int:
        n = max(0, int(n))
        if n <= 0:
            return 0
        with self._lock:
            dropped = min(n, self._size)
            if dropped:
                self._read = (self._read + dropped) % self.capacity
                self._size -= dropped
                self.stats.dropped_oldest_frames += int(dropped)
            return int(dropped)

    def free_frames(self) -> int:
        with self._lock:
            return int(self.capacity - self._size)

    def write_drop_oldest(self, x: np.ndarray) -> int:
        x = np.asarray(x, dtype=np.float32)
        if x.ndim != 2 or x.shape[1] != self.num_channels:
            raise ValueError(f"expected [T,C] with C={self.num_channels}, got {x.shape}")

        n = int(x.shape[0])
        if n <= 0:
            return 0

        if n >= self.capacity:
            x = x[-self.capacity :]
            n = self.capacity

        with self._lock:
            free = self.capacity - self._size
            dropped = max(0, n - free)
            if dropped:
                self._read = (self._read + dropped) % self.capacity
                self._size -= dropped
                self.stats.dropped_oldest_frames += int(dropped)

            end = self._write + n
            if end <= self.capacity:
                self._buf[self._write : end, :] = x
            else:
                n1 = self.capacity - self._write
                self._buf[self._write :, :] = x[:n1, :]
                self._buf[: end % self.capacity, :] = x[n1:, :]

            self._write = end % self.capacity
            self._size += n
            self.stats.write_calls += 1
            return int(dropped)

    def read_into(self, out: np.ndarray) -> int:
        out = np.asarray(out, dtype=np.float32)
        if out.ndim != 2 or out.shape[1] != self.num_channels:
            raise ValueError(f"expected out [T,C] with C={self.num_channels}, got {out.shape}")

        n = int(out.shape[0])
        if n <= 0:
            return 0

        out[:] = 0.0
        with self._lock:
            take = min(n, self._size)
            if take > 0:
                end = self._read + take
                if end <= self.capacity:
                    out[:take, :] = self._buf[self._read : end, :]
                else:
                    n1 = self.capacity - self._read
                    out[:n1, :] = self._buf[self._read :, :]
                    out[n1:take, :] = self._buf[: end % self.capacity, :]
                self._read = end % self.capacity
                self._size -= take

            if take < n:
                self.stats.underrun_frames += int(n - take)

            self.stats.read_calls += 1
            return int(take)
