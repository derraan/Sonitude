from __future__ import annotations

from dataclasses import dataclass

from .estimate_delay import DelayBandEstimate


@dataclass(frozen=True)
class PolarityDecision:
    polarity: int
    ambiguous: bool
    reason: str


def polarity_from_delay_peak(delay_estimate: DelayBandEstimate, threshold: float = 0.20) -> PolarityDecision:
    peak = delay_estimate.peak_signed_corr
    if abs(peak) < threshold:
        return PolarityDecision(
            polarity=1,
            ambiguous=True,
            reason=f"abs(corr)={abs(peak):.3f} below threshold={threshold:.3f}",
        )
    return PolarityDecision(
        polarity=(1 if peak >= 0.0 else -1),
        ambiguous=False,
        reason=f"signed correlation={peak:.3f}",
    )
