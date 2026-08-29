"""Typed data structures shared across the test bench (no Qt, no DSP)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class WavMetadata:
    """Metadata describing a WAV file, as shown before processing."""

    path: Path
    filename: str
    sample_rate_hz: int
    channels: int
    bit_depth: int
    subtype: str
    duration_s: float
    num_samples: int

    def as_dict(self) -> dict:
        return {
            "path": str(self.path),
            "filename": self.filename,
            "sample_rate_hz": self.sample_rate_hz,
            "channels": self.channels,
            "bit_depth": self.bit_depth,
            "subtype": self.subtype,
            "duration_s": self.duration_s,
            "num_samples": self.num_samples,
        }


@dataclass
class ValidationResult:
    """Result of validating a candidate input WAV file for the 6-channel pipeline."""

    ok: bool
    errors: list[str] = field(default_factory=list)
    metadata: WavMetadata | None = None


@dataclass
class SteeringEvent:
    """One commanded steering change, matching sonitude_wav_replay's script format.

    width_deg (0-180, default 0) is the test bench's directivity-blend
    definition of beam "width" — see testbench/README.md, "Steering width
    definition". The underlying DelaySumBeamformer has no native width
    parameter; this value only has meaning through the C++ tools' blend
    logic (src/tools/wav_replay.cpp, src/tools/stream_process.cpp).
    """

    time_s: float
    azimuth_deg: float
    elevation_deg: float
    width_deg: float = 0.0


@dataclass
class TestPaths:
    """Filesystem layout for a single reproducible test result."""

    test_id: str
    root: Path
    input_wav: Path
    processed_wav: Path
    processed_stereo_wav: Path
    beamformed_wav: Path
    suppressed_wav: Path
    raw_preview_wav: Path
    residual_beamform_wav: Path
    residual_limiter_wav: Path
    metadata_json: Path
    metrics_json: Path
    steering_script: Path
