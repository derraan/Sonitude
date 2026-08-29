"""Typed data structures shared across the test bench (no Qt, no DSP)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class AudioMetadata:
    """Metadata describing a decoded audio file, as shown before processing."""

    path: Path
    filename: str
    sample_rate_hz: int
    channels: int
    bit_depth: int
    subtype: str
    duration_s: float
    num_samples: int
    container: str = "wav"
    decoder: str = "libsndfile/soundfile"

    def as_dict(self) -> dict:
        return {
            "path": str(self.path),
            "filename": self.filename,
            "container": self.container,
            "sample_rate_hz": self.sample_rate_hz,
            "channels": self.channels,
            "bit_depth": self.bit_depth,
            "subtype": self.subtype,
            "duration_s": self.duration_s,
            "num_samples": self.num_samples,
            "decoder": self.decoder,
        }


# Compatibility alias.
WavMetadata = AudioMetadata


@dataclass
class ValidationResult:
    """Result of validating a candidate input file for the 6-channel pipeline."""

    ok: bool
    errors: list[str] = field(default_factory=list)
    metadata: AudioMetadata | None = None
    error_kind: str | None = None  # codec | codec_capability | dsp_input | None


@dataclass
class SteeringEvent:
    """One commanded steering change, matching sonitude_wav_replay's script format.

    directivity_blend_deg (0-180, default 0) is a test-bench mix of the
    delay-and-sum output toward the six-microphone average. It is NOT a
    measured physical beamwidth / HPBW. width_deg is a compatibility alias.
    """

    time_s: float
    azimuth_deg: float
    elevation_deg: float
    width_deg: float = 0.0

    @property
    def directivity_blend_deg(self) -> float:
        return self.width_deg

    @directivity_blend_deg.setter
    def directivity_blend_deg(self, value: float) -> None:
        self.width_deg = value


@dataclass
class BinauralRequest:
    enabled: bool = False
    backend: str | None = None
    azimuth_deg: float = 0.0
    elevation_deg: float = 0.0
    follow_beamformer_steering: bool = False

    def as_dict(self) -> dict:
        return {
            "enabled": self.enabled,
            "backend": self.backend,
            "azimuth_deg": self.azimuth_deg,
            "elevation_deg": self.elevation_deg,
            "follow_beamformer_steering": self.follow_beamformer_steering,
        }


@dataclass
class SuppressorRequest:
    """Live conservative suppressor tuning (protocol v3 per audio block)."""

    ambient_floor_linear: float = 0.25
    fade_ms: float = 120.0
    activity_threshold: float = 0.03
    confidence_threshold: float = 0.6
    envelope_attack_coeff: float = 0.35
    envelope_release_coeff: float = 0.01
    confidence: float = 1.0
    focus_active: bool = True

    def as_dict(self) -> dict:
        return {
            "ambient_floor_linear": self.ambient_floor_linear,
            "fade_ms": self.fade_ms,
            "activity_threshold": self.activity_threshold,
            "confidence_threshold": self.confidence_threshold,
            "envelope_attack_coeff": self.envelope_attack_coeff,
            "envelope_release_coeff": self.envelope_release_coeff,
            "confidence": self.confidence,
            "focus_active": self.focus_active,
        }


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
    binaural_wav: Path | None = None
    processed_export: Path | None = None
    runtime_config_copy: Path | None = None
    input_metadata_json: Path | None = None
