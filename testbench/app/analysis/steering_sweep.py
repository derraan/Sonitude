"""Objective steering test: does the beamformer actually focus where it's told to?

Commanding an azimuth and observing that a command was sent proves nothing
about whether the algorithm responded. This module runs the SAME unmodified
algorithm (via sonitude_wav_replay) at a sweep of candidate azimuths against
one recording, measures the beamformed output energy at each candidate, and
reports the azimuth of maximum energy as a measured "response peak" —
i.e. the direction the beamformer's own gain pattern favors most for this
signal, given its actual mic geometry and delay-and-sum weights.

This is an energy-based beam-response sweep, not a directional estimate from
an independent DOA algorithm — no such component exists in the pipeline (see
app/analysis/steering_error.py). It requires a recording of a single,
reasonably stationary, dominant sound source at a known "expected" angle to
be meaningful: on a recording with multiple simultaneous sources, or one
dominated by diffuse/ambient noise, the energy peak may not correspond to
any single source and should not be read as a DOA measurement. It is a
regression check ("does the array's response still peak near where a known
test source sits") rather than a general-purpose localization tool.
"""

from __future__ import annotations

import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from app.audio_io import wav_loader
from app.processing.batch_adapter import run_wav_replay
from app.storage.models import SteeringEvent


@dataclass
class SweepPoint:
    azimuth_deg: float
    output_rms_dbfs: float


@dataclass
class SteeringSweepResult:
    expected_azimuth_deg: float | None
    measured_peak_azimuth_deg: float
    error_deg: float | None
    sweep: list[SweepPoint]

    def as_dict(self) -> dict:
        return {
            "expected_azimuth_deg": self.expected_azimuth_deg,
            "measured_peak_azimuth_deg": self.measured_peak_azimuth_deg,
            "error_deg": self.error_deg,
            "sweep": [{"azimuth_deg": p.azimuth_deg, "output_rms_dbfs": p.output_rms_dbfs} for p in self.sweep],
        }


def run_steering_sweep(
    input_wav: str | Path,
    config_path: str | Path,
    *,
    expected_azimuth_deg: float | None = None,
    azimuth_range_deg: tuple[float, float] = (-90.0, 90.0),
    step_deg: float = 15.0,
    binary_path: str | Path | None = None,
) -> SteeringSweepResult:
    """Sweep commanded azimuth over a range and measure beamformed output energy.

    Reuses sonitude_wav_replay unchanged: one full-file render per candidate
    azimuth, reading back only the pre-suppression beamformed tap (so
    suppression/limiter never distort the energy comparison across angles).
    """
    candidates = np.arange(azimuth_range_deg[0], azimuth_range_deg[1] + step_deg / 2.0, step_deg)
    sweep: list[SweepPoint] = []

    with tempfile.TemporaryDirectory(prefix="sonitude_steering_sweep_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        for azimuth in candidates:
            azimuth_float = float(azimuth)
            events = [SteeringEvent(time_s=0.0, azimuth_deg=azimuth_float, elevation_deg=0.0, width_deg=0.0)]
            output_dir = tmp_path / f"az_{azimuth_float:.1f}"
            result = run_wav_replay(
                input_wav,
                config_path,
                events,
                output_dir,
                enable_suppression=False,
                disable_limiter=True,
                binary_path=binary_path,
            )
            beamformed, _sample_rate = wav_loader.load_wav(result.beamformed_wav)
            rms = float(np.sqrt(np.mean(np.square(beamformed)) + 1e-12))
            sweep.append(SweepPoint(azimuth_deg=azimuth_float, output_rms_dbfs=20.0 * np.log10(rms + 1e-12)))

    peak = max(sweep, key=lambda p: p.output_rms_dbfs)
    error = (peak.azimuth_deg - expected_azimuth_deg) if expected_azimuth_deg is not None else None
    return SteeringSweepResult(
        expected_azimuth_deg=expected_azimuth_deg,
        measured_peak_azimuth_deg=peak.azimuth_deg,
        error_deg=error,
        sweep=sweep,
    )
