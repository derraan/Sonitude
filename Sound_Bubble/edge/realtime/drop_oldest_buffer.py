from __future__ import annotations

import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Deque, Generic, Optional, TypeVar

T = TypeVar("T")


@dataclass
class DropStats:
    dropped_oldest: int = 0


class DropOldestBuffer(Generic[T]):
    """
    Fixed-capacity buffer with drop-oldest semantics.

    Producer: push() never blocks. If full, discards the oldest item.
    Consumer: pop(timeout) waits for data; returns None on timeout or when closed.
    """

    def __init__(self, capacity: int) -> None:
        self._cap = max(1, int(capacity))
        self._q: Deque[T] = deque()
        self._cv = threading.Condition()
        self.stats = DropStats()
        self._closed = False

    @property
    def capacity(self) -> int:
        return self._cap

    def size(self) -> int:
        with self._cv:
            return len(self._q)

    def qsize(self) -> int:
        return self.size()

    def close(self) -> None:
        with self._cv:
            self._closed = True
            self._cv.notify_all()

    def push(self, item: T) -> None:
        with self._cv:
            if self._closed:
                return
            if len(self._q) >= self._cap:
                self._q.popleft()
                self.stats.dropped_oldest += 1
            self._q.append(item)
            self._cv.notify(1)

    def pop(self, timeout: float | None = None) -> Optional[T]:
        deadline = None if timeout is None else (time.monotonic() + max(0.0, float(timeout)))
        with self._cv:
            while True:
                if self._q:
                    return self._q.popleft()
                if self._closed:
                    return None
                if deadline is None:
                    self._cv.wait()
                else:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return None
                    self._cv.wait(timeout=remaining)

    def pop_latest(self, timeout: float | None = None):
        item = self.pop(timeout=timeout)
        if item is None:
            return None, 0

        drained = 0
        while True:
            nxt = self.pop(timeout=0.0)
            if nxt is None:
                break
            item = nxt
            drained += 1

        return item, drained

