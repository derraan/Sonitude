from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class RealtimeSchedulingResult:
    requested_policy: str
    requested_priority: int
    applied: bool
    message: str


def try_set_realtime_fifo(priority: int) -> RealtimeSchedulingResult:
    """
    Best-effort attempt to put the *current process* into SCHED_FIFO.

    Must be called as early as possible (before audio streams start).
    """
    prio = int(priority)
    if prio <= 0:
        return RealtimeSchedulingResult(
            requested_policy="SCHED_FIFO",
            requested_priority=prio,
            applied=False,
            message="skipped (priority<=0)",
        )
    try:
        param = os.sched_param(prio)
        os.sched_setscheduler(0, os.SCHED_FIFO, param)
        return RealtimeSchedulingResult(
            requested_policy="SCHED_FIFO",
            requested_priority=prio,
            applied=True,
            message="applied",
        )
    except PermissionError:
        return RealtimeSchedulingResult(
            requested_policy="SCHED_FIFO",
            requested_priority=prio,
            applied=False,
            message="permission denied (run with sudo/chrt or grant CAP_SYS_NICE)",
        )
    except Exception as exc:
        return RealtimeSchedulingResult(
            requested_policy="SCHED_FIFO",
            requested_priority=prio,
            applied=False,
            message=f"failed ({exc})",
        )
