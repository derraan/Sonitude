"""Adapter around the existing ``sonitude_wav_replay`` batch CLI tool.

No DSP is implemented here. This module only decodes supported audio files
to a canonical WAV the C++ tool can read, builds a subprocess command line,
writes the steering script the tool expects, and interprets the resulting
WAV files.
"""

from __future__ import annotations

import json
import subprocess
from collections.abc import Callable
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path

import numpy as np

from app.audio_io.audio_loader import load_audio, probe_audio, validate_dsp_input
from app.audio_io.downmix import ear_cup_stereo_preview
from app.audio_io.exporter import export_pcm
from app.audio_io.stream_io import DEFAULT_BLOCK_FRAMES, iter_mapped_blocks
from app.processing.sonitude_binary_locator import find_binary
from app.processing.stream_adapter import StreamProcessor, StreamProtocolError
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
    streaming: bool = False
    frames: int = 0
    sample_rate_hz: int | None = None


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


@lru_cache(maxsize=8)
def wav_replay_supports_binaural_direction_cli(binary: str) -> bool:
    """True when this wav_replay binary accepts --binaural-follow-steering.

    PR #32 builds advertise --output-binaural but reject direction overrides.
    Merged HRTF trees accept both. Probe --help so an older exe is not fed
    unknown flags that abort the whole job before any processed WAV exists.
    """
    try:
        proc = subprocess.run(
            [binary, "--help"],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return "--binaural-follow-steering" in f"{proc.stdout}\n{proc.stderr}"


def binaural_cli_args(
    request: BinauralRequest,
    binaural_wav: str | Path,
    *,
    include_direction_overrides: bool = True,
) -> list[str]:
    """CLI flags that map GUI binaural controls onto sonitude_wav_replay."""
    if not request.enabled:
        return []
    args = ["--output-binaural", str(binaural_wav)]
    if request.backend:
        args += ["--binaural-backend", request.backend]
    if not include_direction_overrides:
        return args
    if request.follow_beamformer_steering:
        args.append("--binaural-follow-steering")
    else:
        args += [
            "--binaural-fixed-direction",
            "--binaural-azimuth",
            f"{request.azimuth_deg:.6f}",
            "--binaural-elevation",
            f"{request.elevation_deg:.6f}",
        ]
    return args


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
    suppression_backend: str | None = None,
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
    if suppression_backend:
        command += ["--suppression-backend", suppression_backend]
    if disable_limiter:
        command.append("--disable-limiter")
    command += binaural_cli_args(
        binaural_request,
        binaural_wav,
        include_direction_overrides=wav_replay_supports_binaural_direction_cli(str(binary)),
    )

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
        streaming=False,
    )


def _steering_at(events: list[SteeringEvent], time_s: float) -> SteeringEvent:
    chosen = events[0]
    for event in events:
        if event.time_s <= time_s:
            chosen = event
    return chosen


def run_stream_batch(
    input_wav: str | Path,
    config_path: str | Path,
    steering_events: list[SteeringEvent],
    output_dir: str | Path,
    *,
    suppression: SuppressionMode | str = SuppressionMode.AUTO,
    disable_limiter: bool = False,
    binaural: BinauralRequest | None = None,
    active_channel_map: list[int] | None = None,
    binary_path: str | Path | None = None,
    build_dir: str | Path | None = None,
    block_frames: int = DEFAULT_BLOCK_FRAMES,
    progress_callback: Callable[[int, int], None] | None = None,
) -> BatchResult:
    """Process a recording through sonitude_stream_process without loading it all.

    Used for long captures. Writes PCM_16 stereo WAVs Qt can play. Does not
    produce wav_replay diagnostic taps (beamformed/suppressed).
    """
    import soundfile as sf

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    input_path = Path(input_wav)
    metadata = probe_audio(input_path)
    channel_map = list(active_channel_map) if active_channel_map else [0, 1, 2, 3, 4, 5]
    events = steering_events or [SteeringEvent(0.0, 0.0, 0.0, 0.0)]
    binaural_request = binaural or BinauralRequest()
    mode = parse_suppression_mode(suppression)
    binary = Path(binary_path) if binary_path else find_binary("sonitude_stream_process", build_dir)

    output_wav = output_dir / "processed.wav"
    processed_stereo = output_dir / "processed_stereo.wav"
    raw_preview = output_dir / "raw_preview_stereo.wav"
    binaural_wav = output_dir / "binaural_stereo.wav"
    write_steering_script(events, output_dir / "steering_script.csv")

    processor = StreamProcessor(
        config_path,
        sample_rate_hz=metadata.sample_rate_hz,
        max_block_frames=block_frames,
        suppression=mode,
        disable_limiter=disable_limiter,
        binary_path=binary,
        build_dir=build_dir,
    )
    frames_written = 0
    total_frames = int(metadata.num_samples)
    last_pct = -1
    try:
        with (
            sf.SoundFile(
                str(processed_stereo),
                mode="w",
                samplerate=metadata.sample_rate_hz,
                channels=2,
                subtype="PCM_16",
            ) as stereo_out,
            sf.SoundFile(
                str(output_wav),
                mode="w",
                samplerate=metadata.sample_rate_hz,
                channels=1,
                subtype="PCM_16",
            ) as mono_out,
            sf.SoundFile(
                str(raw_preview),
                mode="w",
                samplerate=metadata.sample_rate_hz,
                channels=2,
                subtype="PCM_16",
            ) as preview_out,
        ):
            binaural_out = None
            if binaural_request.enabled:
                binaural_out = sf.SoundFile(
                    str(binaural_wav),
                    mode="w",
                    samplerate=metadata.sample_rate_hz,
                    channels=2,
                    subtype="PCM_16",
                )
            try:
                time_s = 0.0
                dt = 1.0 / float(metadata.sample_rate_hz) if metadata.sample_rate_hz else 0.0
                for block in iter_mapped_blocks(input_path, channel_map, block_frames=block_frames):
                    event = _steering_at(events, time_s)
                    processor.send_block(
                        block,
                        event.azimuth_deg,
                        event.elevation_deg,
                        True,
                        width_deg=event.width_deg,
                        binaural_enabled=binaural_request.enabled,
                        binaural_follow_steering=binaural_request.follow_beamformer_steering,
                        binaural_azimuth_deg=binaural_request.azimuth_deg,
                        binaural_elevation_deg=binaural_request.elevation_deg,
                        binaural_backend=binaural_request.backend,
                    )
                    processed, _seq = processor.recv_block()
                    stereo_out.write(processed)
                    mono_out.write(processed[:, :1])
                    preview_out.write(ear_cup_stereo_preview(block, channel_map))
                    if binaural_out is not None:
                        binaural_out.write(processed)
                    frames_written += len(block)
                    time_s += len(block) * dt
                    if progress_callback and total_frames:
                        pct = min(100, int(frames_written * 100 / total_frames))
                        if pct != last_pct:
                            last_pct = pct
                            progress_callback(frames_written, total_frames)
            finally:
                if binaural_out is not None:
                    binaural_out.close()
    except StreamProtocolError as exc:
        raise BatchProcessingError(str(exc)) from exc
    finally:
        processor.close()

    resolved = parse_sonitude_resolved(processor.stderr_text())
    return BatchResult(
        output_wav=output_wav,
        beamformed_wav=output_dir / "beamformed.wav",
        suppressed_wav=output_dir / "suppressed.wav",
        stdout="",
        stderr=processor.stderr_text(),
        command=[str(binary)],
        decoded_input_wav=input_path,
        binaural_wav=binaural_wav if binaural_request.enabled and binaural_wav.exists() else None,
        resolved=resolved,
        suppression_requested=mode.value,
        suppression_resolved=resolved.get("suppression_resolved") if resolved else None,
        streaming=True,
        frames=frames_written,
        sample_rate_hz=metadata.sample_rate_hz,
    )
