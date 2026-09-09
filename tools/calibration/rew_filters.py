from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np


@dataclass(frozen=True)
class RewFilter:
    ftype: str
    freq_hz: float
    gain_db: float
    q: float


@dataclass(frozen=True)
class RewFilterParseResult:
    verbatim: list[RewFilter]
    guarded: list[RewFilter]
    dropped_reasons: list[str]
    worst_case_cumulative_boost_db: float


_ALLOWED_TYPES = {"PK", "LS", "HS", "LP", "HP"}


def parse_rew_filter_txt(
    path: str | Path,
    max_q: float = 4.0,
    max_boost_db: float = 6.0,
    min_freq_hz: float = 100.0,
) -> RewFilterParseResult:
    lines = Path(path).read_text(encoding="utf-8", errors="ignore").splitlines()
    verbatim: list[RewFilter] = []
    for line in lines:
        cols = line.strip().split()
        if len(cols) < 7:
            continue
        # formatted file row:
        # idx True Auto PK 163.5 2.80 4.776 ...
        if not cols[0].isdigit():
            continue
        ftype = cols[3].upper()
        if ftype == "NONE":
            continue
        if ftype not in _ALLOWED_TYPES:
            continue
        try:
            freq_hz = float(cols[4])
            gain_db = float(cols[5]) if ftype in {"PK", "LS", "HS"} else 0.0
            q = float(cols[6]) if len(cols) > 6 else 0.707
        except ValueError:
            continue
        verbatim.append(RewFilter(ftype=ftype, freq_hz=freq_hz, gain_db=gain_db, q=q))

    guarded: list[RewFilter] = []
    dropped: list[str] = []
    for f in verbatim:
        if f.freq_hz < min_freq_hz:
            dropped.append(f"drop {f.ftype}@{f.freq_hz:.1f}Hz below min_freq")
            continue
        q = min(f.q, max_q)
        if q != f.q:
            dropped.append(f"cap Q {f.ftype}@{f.freq_hz:.1f}Hz {f.q:.3f}->{q:.3f}")
        gain_db = float(np.clip(f.gain_db, -max_boost_db, max_boost_db))
        if gain_db != f.gain_db:
            dropped.append(f"cap gain {f.ftype}@{f.freq_hz:.1f}Hz {f.gain_db:.2f}->{gain_db:.2f} dB")
        guarded.append(RewFilter(ftype=f.ftype, freq_hz=f.freq_hz, gain_db=gain_db, q=q))

    cumulative_linear = sum(10.0 ** (max(0.0, f.gain_db) / 20.0) for f in guarded)
    worst_case = 20.0 * np.log10(max(cumulative_linear, 1.0))
    return RewFilterParseResult(
        verbatim=verbatim,
        guarded=guarded,
        dropped_reasons=dropped,
        worst_case_cumulative_boost_db=float(worst_case),
    )
