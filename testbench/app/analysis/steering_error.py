"""Steering direction comparison: commanded vs. estimated.

The Sonitude algorithm today only CONSUMES a commanded steering target (a
script, in batch mode, or the GUI dial, in real-time mode). Nothing in the
pipeline currently ESTIMATES a direction-of-arrival from the signal — the
mock DOA provider replays scripted values as if they were ground truth, it
does not infer them. So "steering error" here is only ever computable when
the caller supplies its own estimate; when none is supplied, the estimate
fields are left as None rather than fabricated, and the GUI should show them
as unavailable.
"""

from __future__ import annotations

import csv
from dataclasses import dataclass
from pathlib import Path

from app.storage.models import SteeringEvent


def parse_steering_script(path: str | Path) -> list[SteeringEvent]:
    """Parse a steering script in the same format sonitude_wav_replay reads.

    The 4th column (width_deg) is optional for backward compatibility with
    older 3-column scripts; it defaults to 0.0 (fully directional) when absent.
    """
    events: list[SteeringEvent] = []
    with open(path, encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            parts = next(csv.reader([line.replace("\t", ",")]))
            if len(parts) < 3:
                continue
            try:
                width_deg = float(parts[3]) if len(parts) >= 4 and parts[3] != "" else 0.0
                events.append(
                    SteeringEvent(
                        time_s=float(parts[0]),
                        azimuth_deg=float(parts[1]),
                        elevation_deg=float(parts[2]),
                        width_deg=width_deg,
                    )
                )
            except ValueError:
                continue  # header line
    if not events:
        events.append(SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0, width_deg=0.0))
    return sorted(events, key=lambda e: e.time_s)


def commanded_direction_at(events: list[SteeringEvent], time_s: float) -> SteeringEvent:
    """The commanded target in effect at a given time (last event at/before it)."""
    active = events[0]
    for event in events:
        if event.time_s <= time_s:
            active = event
        else:
            break
    return active


@dataclass
class SteeringErrorSample:
    time_s: float
    commanded_azimuth_deg: float
    commanded_elevation_deg: float
    estimated_azimuth_deg: float | None
    estimated_elevation_deg: float | None
    azimuth_error_deg: float | None
    elevation_error_deg: float | None
    estimate_available: bool


def compute_steering_error(
    commanded_events: list[SteeringEvent],
    estimated_events: list[SteeringEvent] | None = None,
) -> list[SteeringErrorSample]:
    """Build a commanded-vs-estimated timeline.

    If ``estimated_events`` is None (the current default, since no estimator
    exists in the pipeline), every sample reports ``estimate_available=False``
    and error fields as None rather than a computed number.
    """
    if not estimated_events:
        return [
            SteeringErrorSample(
                time_s=event.time_s,
                commanded_azimuth_deg=event.azimuth_deg,
                commanded_elevation_deg=event.elevation_deg,
                estimated_azimuth_deg=None,
                estimated_elevation_deg=None,
                azimuth_error_deg=None,
                elevation_error_deg=None,
                estimate_available=False,
            )
            for event in commanded_events
        ]

    samples: list[SteeringErrorSample] = []
    for estimate in estimated_events:
        commanded = commanded_direction_at(commanded_events, estimate.time_s)
        samples.append(
            SteeringErrorSample(
                time_s=estimate.time_s,
                commanded_azimuth_deg=commanded.azimuth_deg,
                commanded_elevation_deg=commanded.elevation_deg,
                estimated_azimuth_deg=estimate.azimuth_deg,
                estimated_elevation_deg=estimate.elevation_deg,
                azimuth_error_deg=estimate.azimuth_deg - commanded.azimuth_deg,
                elevation_error_deg=estimate.elevation_deg - commanded.elevation_deg,
                estimate_available=True,
            )
        )
    return samples
