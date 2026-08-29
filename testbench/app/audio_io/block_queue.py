"""Small bounded audio-block queue with newest-data-wins (drop-oldest) behaviour."""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass
from threading import Condition
from typing import Generic, TypeVar

T = TypeVar("T")

DEFAULT_CAPACITY = 3


@dataclass(frozen=True)
class QueueSnapshot:
    capacity: int
    depth: int
    dropped_blocks: int
    overrun: bool


class DropOldestQueue(Generic[T]):
    """Thread-safe bounded queue. When full, discard the oldest item and enqueue the newest.

    Capacity is configurable. The default of 3 is a starting point for the test
    bench, not a claim that 2 or 3 is universally optimal for all hardware.
    """

    def __init__(self, capacity: int = DEFAULT_CAPACITY) -> None:
        if capacity < 1:
            raise ValueError("capacity must be >= 1")
        self._capacity = capacity
        self._items: deque[T] = deque()
        self._cond = Condition()
        self._dropped_blocks = 0

    @property
    def capacity(self) -> int:
        return self._capacity

    def put(self, item: T) -> bool:
        """Enqueue ``item``. Returns True if an older block was dropped."""
        with self._cond:
            dropped = False
            if len(self._items) >= self._capacity:
                self._items.popleft()
                self._dropped_blocks += 1
                dropped = True
            self._items.append(item)
            self._cond.notify()
            return dropped

    def get_nowait(self) -> T | None:
        with self._cond:
            if not self._items:
                return None
            return self._items.popleft()

    def get(self, timeout: float | None = None) -> T | None:
        """Pop the oldest item, waiting up to ``timeout`` seconds. None on timeout."""
        deadline = None if timeout is None else time.monotonic() + timeout
        with self._cond:
            while not self._items:
                if deadline is None:
                    self._cond.wait()
                    continue
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._cond.wait(remaining)
            return self._items.popleft()

    def snapshot(self) -> QueueSnapshot:
        with self._cond:
            depth = len(self._items)
            dropped = self._dropped_blocks
        return QueueSnapshot(
            capacity=self._capacity,
            depth=depth,
            dropped_blocks=dropped,
            overrun=dropped > 0,
        )
