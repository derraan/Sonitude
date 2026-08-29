"""Adapter around the existing ``sonitude_wav_replay`` batch CLI tool.

No DSP is implemented here. This module only decodes supported audio files
to a canonical WAV the C++ tool can read, builds a subprocess command line,
writes the steering script the tool expects, and interprets the resulting
WAV files.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from app.audio_io.audio_loader import load_audio, validate_dsp_input
from app.audio_io.exporter import export_pcm
from app.processing.sonitude_binary_locator import find_binary
from app.processing.suppression import SuppressionMode, cli_args_for, parse_suppression_mode
from app.storage.models import BinauralRequest, SteeringEvent


class BatchProcessingError(RuntimeError):
    """Raised when sonitude_wav_replay exits with a non-zero status."""


@dataclass
class BatchResult:
    output_wav: Path
    beamformed_wav: Path
    suppressed_wav: Path
    stdout: str
    stderr: str
    command: list[str]
    decoded_input_wav: Path
    binaural_wav: Path | None = None
    resolved: dict = field(default_factory=dict)
    suppression_requested: str = "auto"
    suppression_resolved: bool | None = None


def write_steering_script(events: list[SteeringEvent], path: str | Path) -> Path:
    """Write a steering script in the format sonitude_wav_replay expects.

    Format (see src/tools/wav_replay.cpp LoadSteeringScript): comma-separated
    ``time_s,azimuth_deg,elevation_deg[,directivity_blend_deg]`` lines.
    """
    path = Path(path)
    if not events:
        events = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0, width_deg=0.0)]
    lines = ["# time_s,azimuth_deg,elevation_deg,directivity_blend_deg"]
    for event in sorted(events, key=lambda e: e.time_s):
        lines.append(f"{event.time_s},{event.azimuth_deg},{event.elevation_deg},{event.width_deg}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def parse_sonitude_resolved(stderr: str) -> dict:
    """Parse the ``sonitude_resolved {...}`` JSON line from C++ stderr, if present."""
    for line in stderr.splitlines():
        stripped = line.strip()
        if stripped.startswith("sonitude_resolved"):
            payload = stripped[len("sonitude_resolved") :].strip()
            try:
                parsed = json.loads(payload)
            except json.JSONDecodeError:
                return {}
            return parsed if isinstance(parsed, dict) else {}
    return {}


def _prepare_decoded_wav(
    input_path: Path,
    output_dir: Path,
    *,
    expected_sample_rate_hz: int | None,
    active_channel_map: list[int] | None,
) -> Path:
    """Decode any supported container to float32 WAV for the C++ WAV-only tool.

    Preserves original channel count and order. DSP validation (channel map /
    six-microphone layout) is separate from codec decode.
    """
    pcm, metadata = load_audio(input_path)
    array_validation = validate_dsp_input(
        pcm,
        expected_sample_rate_hz=expected_sample_rate_hz,
        actual_sample_rate_hz=metadata.sample_rate_hz,
        active_channel_map=active_channel_map,
    )
    if not array_validation.ok:
        raise BatchProcessingError("; ".join(array_validation.errors))
    decoded = output_dir / "input_decoded.wav"
    export_pcm(decoded, pcm.astype(np.float32, copy=False), metadata.sample_rate_hz, container="wav")
    return decoded


def run_wav_replay(
    input_wav: str | Path,
    config_path: str | Path,
    steering_events: list[SteeringEvent],
    output_dir: str | Path,
    *,
    suppression: SuppressionMode | str = SuppressionMode.AUTO,
    enable_suppression: bool | None = None,
    disable_limiter: bool = False,
    binaural: BinauralRequest | None = None,
    expected_sample_rate_hz: int | None = None,
    active_channel_map: list[int] | None = None,
    binary_path: str | Path | None = None,
    build_dir: str | Path | None = None,
) -> BatchResult:
    """Run sonitude_wav_replay on one decoded 6-channel WAV file.

    ``input_wav`` may be WAV/FLAC/MP3; this adapter decodes it first. Changing
    the later Python export container does not change this DSP invocation.
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    input_path = Path(input_wav)

    mode = parse_suppression_mode(suppression)
    if enable_suppression is True:
        mode = SuppressionMode.ON
    elif enable_suppression is False:
        mode = SuppressionMode.OFF

    decoded_input = _prepare_decoded_wav(
        input_path,
        output_dir,
        expected_sample_rate_hz=expected_sample_rate_hz,
        active_channel_map=active_channel_map,
    )

    binary = Path(binary_path) if binary_path else find_binary("sonitude_wav_replay", build_dir)
    script_path = output_dir / "steering_script.csv"
    write_steering_script(steering_events, script_path)

    output_wav = output_dir / "processed.wav"
    beamformed_wav = output_dir / "beamformed.wav"
    suppressed_wav = output_dir / "suppressed.wav"
    binaural_wav = output_dir / "binaural_stereo.wav"
    binaural_request = binaural or BinauralRequest()

    command = [
        str(binary),
        "--input", str(decoded_input),
        "--config", str(config_path),
        "--script", str(script_path),
        "--output", str(output_wav),
        "--output-beamformed", str(beamformed_wav),
        "--output-suppressed", str(suppressed_wav),
        *cli_args_for(mode),
    ]
    if disable_limiter:
        command.append("--disable-limiter")
    if binaural_request.enabled:
        command += ["--output-binaural", str(binaural_wav)]
        if binaural_request.backend:
            command += ["--binaural-backend", binaural_request.backend]

    proc = subprocess.run(command, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise BatchProcessingError(
            f"sonitude_wav_replay failed (exit {proc.returncode}): {proc.stderr.strip()}"
        )

    resolved = parse_sonitude_resolved(proc.stderr)
    binaural_path = binaural_wav if binaural_request.enabled and binaural_wav.exists() else None
    return BatchResult(
        output_wav=output_wav,
        beamformed_wav=beamformed_wav,
        suppressed_wav=suppressed_wav,
        stdout=proc.stdout,
        stderr=proc.stderr,
        command=command,
        decoded_input_wav=decoded_input,
        binaural_wav=binaural_path,
        resolved=resolved,
        suppression_requested=mode.value,
        suppression_resolved=resolved.get("suppression_resolved") if resolved else None,
    )
