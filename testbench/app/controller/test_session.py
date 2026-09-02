"""Lightweight session state for the Recorded Data tab (no Qt, no DSP)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from app.storage.models import SteeringEvent


@dataclass
class BatchTestSession:
    input_paths: list[Path] = field(default_factory=list)
    config_path: Path | None = None
    steering_events: list[SteeringEvent] = field(default_factory=lambda: [SteeringEvent(0.0, 0.0, 0.0)])
    enable_suppression: bool = False
    disable_limiter: bool = False
    completed_test_ids: list[str] = field(default_factory=list)
