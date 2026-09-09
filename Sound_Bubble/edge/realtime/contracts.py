from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class RealtimeContract:
    """
    Hard invariants to prevent unbounded latency accumulation.

    - capture buffer: bounded by max_capture_blocks; overload drops oldest (keeps newest audio)
    - output buffer: bounded by max_output_ahead_sec; overload trims oldest queued audio to recover
    """

    max_capture_blocks: int = 4
    max_output_ahead_sec: float = 0.8
    target_output_ahead_sec: float = 0.4
    rt_fifo_priority: int = 0

    def validate(self) -> "RealtimeContract":
        if self.max_capture_blocks < 1:
            raise ValueError("max_capture_blocks must be >= 1")
        if self.max_output_ahead_sec <= 0:
            raise ValueError("max_output_ahead_sec must be > 0")
        if self.target_output_ahead_sec <= 0:
            raise ValueError("target_output_ahead_sec must be > 0")
        if self.target_output_ahead_sec >= self.max_output_ahead_sec:
            raise ValueError("target_output_ahead_sec must be < max_output_ahead_sec")
        if self.rt_fifo_priority < 0:
            raise ValueError("rt_fifo_priority must be >= 0")
        return self

