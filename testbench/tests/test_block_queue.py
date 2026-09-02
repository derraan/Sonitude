"""Drop-oldest / newest-wins queue. These tests fail if the queue drops newest instead."""

from __future__ import annotations

from app.audio_io.block_queue import DropOldestQueue


def test_drop_oldest_keeps_newest_when_over_capacity() -> None:
    queue = DropOldestQueue[str](capacity=2)
    queue.put("oldest")
    queue.put("middle")
    dropped = queue.put("newest")
    assert dropped is True
    # Drop-newest would return oldest then middle. Drop-oldest must return middle then newest.
    assert queue.get_nowait() == "middle"
    assert queue.get_nowait() == "newest"
    assert queue.get_nowait() is None


def test_capacity_three_discards_the_first_of_four() -> None:
    queue = DropOldestQueue[int](capacity=3)
    for value in (0, 1, 2, 3):
        queue.put(value)
    remaining = [queue.get_nowait() for _ in range(3)]
    assert remaining == [1, 2, 3]
    assert 0 not in remaining


def test_snapshot_reports_depth_dropped_and_overrun() -> None:
    queue = DropOldestQueue[int](capacity=2)
    queue.put(1)
    snap = queue.snapshot()
    assert snap.depth == 1
    assert snap.dropped_blocks == 0
    assert snap.overrun is False
    queue.put(2)
    queue.put(3)
    snap = queue.snapshot()
    assert snap.depth == 2
    assert snap.dropped_blocks == 1
    assert snap.overrun is True


def test_would_fail_if_implementation_dropped_newest() -> None:
    """Negative control: a drop-newest queue would keep 10 and 11, not 11 and 12."""
    queue = DropOldestQueue[int](capacity=2)
    queue.put(10)
    queue.put(11)
    queue.put(12)
    first = queue.get_nowait()
    second = queue.get_nowait()
    assert (first, second) != (10, 11)
    assert (first, second) == (11, 12)
