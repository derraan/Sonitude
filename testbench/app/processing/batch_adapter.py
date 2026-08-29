"""Adapter around the existing ``sonitude_wav_replay`` batch CLI tool.

No DSP is implemented here. This module only builds a subprocess command
line, writes the steering script the tool expects, and interprets the
resulting WAV files.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path

from app.processing.sonitude_binary_locator import find_binary
from app.storage.models import SteeringEvent


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


def write_steering_script(events: list[SteeringEvent], path: str | Path) -> Path:
    """Write a steering script in the format sonitude_wav_replay expects.

    Format (see src/tools/wav_replay.cpp LoadSteeringScript): comma-separated
    ``time_s,azimuth_deg,elevation_deg`` lines, ``#`` comments allowed, first
    non-numeric line is ignored as an optional header.
    """
    path = Path(path)
    if not events:
        events = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0, width_deg=0.0)]
    lines = ["# time_s,azimuth_deg,elevation_deg,width_deg"]
    for event in sorted(events, key=lambda e: e.time_s):
        lines.append(f"{event.time_s},{event.azimuth_deg},{event.elevation_deg},{event.width_deg}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def run_wav_replay(
    input_wav: str | Path,
    config_path: str | Path,
    steering_events: list[SteeringEvent],
    output_dir: str | Path,
    *,
    enable_suppression: bool = False,
    disable_limiter: bool = False,
    binary_path: str | Path | None = None,
    build_dir: str | Path | None = None,
) -> BatchResult:
    """Run sonitude_wav_replay on one 6-channel WAV file.

    Produces the final processed mono WAV plus the two diagnostic taps
    (pre-suppression beamformed, pre-limiter suppressed) that the C++ tool now
    exposes via ``--output-beamformed`` / ``--output-suppressed``.
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    binary = Path(binary_path) if binary_path else find_binary("sonitude_wav_replay", build_dir)
    script_path = output_dir / "steering_script.csv"
    write_steering_script(steering_events, script_path)

    output_wav = output_dir / "processed.wav"
    beamformed_wav = output_dir / "beamformed.wav"
    suppressed_wav = output_dir / "suppressed.wav"

    command = [
        str(binary),
        "--input", str(input_wav),
        "--config", str(config_path),
        "--script", str(script_path),
        "--output", str(output_wav),
        "--output-beamformed", str(beamformed_wav),
        "--output-suppressed", str(suppressed_wav),
    ]
    if enable_suppression:
        command.append("--enable-suppression")
    if disable_limiter:
        command.append("--disable-limiter")

    proc = subprocess.run(command, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise BatchProcessingError(
            f"sonitude_wav_replay failed (exit {proc.returncode}): {proc.stderr.strip()}"
        )

    return BatchResult(
        output_wav=output_wav,
        beamformed_wav=beamformed_wav,
        suppressed_wav=suppressed_wav,
        stdout=proc.stdout,
        stderr=proc.stderr,
        command=command,
    )
