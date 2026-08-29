"""Adapter around sonitude_stream_process using protocol v3."""

from __future__ import annotations

import subprocess
import threading
from pathlib import Path

import numpy as np

from app.processing.protocol import (
    BACKEND_NONE,
    MIC_CHANNELS,
    MSG_SHUTDOWN,
    OUTPUT_HEADER,
    PROTOCOL_VERSION,
    ProtocolError,
    assert_response_sequence,
    backend_id,
    pack_input_header,
    unpack_output_header,
)
from app.processing.sonitude_binary_locator import find_binary
from app.processing.suppression import SuppressionMode, cli_args_for, parse_suppression_mode
from app.storage.models import SuppressorRequest


class StreamProtocolError(ProtocolError):
    """Raised on framing/magic mismatches or unexpected subprocess exit."""


class StreamProcessor:
    """Owns the sonitude_stream_process subprocess for one streaming session."""

    def __init__(
        self,
        config_path: str | Path,
        *,
        sample_rate_hz: int | None = None,
        max_block_frames: int = 8192,
        suppression: SuppressionMode | str = SuppressionMode.AUTO,
        enable_suppression: bool | None = None,
        disable_limiter: bool = False,
        binary_path: str | Path | None = None,
        build_dir: str | Path | None = None,
    ) -> None:
        binary = Path(binary_path) if binary_path else find_binary("sonitude_stream_process", build_dir)
        command = [str(binary), "--config", str(config_path), "--max-block-frames", str(max_block_frames)]
        if sample_rate_hz is not None:
            command += ["--sample-rate", str(sample_rate_hz)]
        mode = parse_suppression_mode(suppression)
        if enable_suppression is True:
            mode = SuppressionMode.ON
        elif enable_suppression is False:
            mode = SuppressionMode.OFF
        command += cli_args_for(mode)
        if disable_limiter:
            command.append("--disable-limiter")

        self._process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self.max_block_frames = max_block_frames
        self.protocol_version = PROTOCOL_VERSION
        self._next_sequence = 0
        self._last_sent_sequence: int | None = None
        self._stderr_chunks: list[str] = []
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._stderr_thread.start()

    def _drain_stderr(self) -> None:
        if self._process.stderr is None:
            return
        for line in iter(self._process.stderr.readline, b""):
            if not line:
                break
            text = line.decode("utf-8", "replace") if isinstance(line, bytes) else line
            self._stderr_chunks.append(text)

    def stderr_text(self) -> str:
        return "".join(self._stderr_chunks)

    def send_block(
        self,
        mic_pcm: np.ndarray,
        azimuth_deg: float,
        elevation_deg: float,
        suppression_focus_active: bool,
        width_deg: float = 0.0,
        *,
        directivity_blend_deg: float | None = None,
        binaural_enabled: bool = False,
        binaural_follow_steering: bool = True,
        binaural_azimuth_deg: float = 0.0,
        binaural_elevation_deg: float = 0.0,
        binaural_backend: str | None = None,
        suppressor: SuppressorRequest | None = None,
    ) -> int:
        """Send one 6-channel float32 block. Returns the sequence number."""
        if self._process.stdin is None:
            raise StreamProtocolError("subprocess stdin is closed")
        if self._process.poll() is not None:
            raise StreamProtocolError(
                f"sonitude_stream_process exited (returncode={self._process.returncode}): {self.stderr_text()}"
            )
        frame_count = mic_pcm.shape[0]
        if mic_pcm.ndim != 2 or mic_pcm.shape[1] != MIC_CHANNELS:
            raise ValueError(f"expected (frames, {MIC_CHANNELS}), got {mic_pcm.shape}")
        blend = width_deg if directivity_blend_deg is None else directivity_blend_deg
        payload = np.ascontiguousarray(mic_pcm, dtype="<f4").tobytes()
        sequence = self._next_sequence
        sup = suppressor or SuppressorRequest()
        focus_active = sup.focus_active if suppressor is not None else suppression_focus_active
        header = pack_input_header(
            sequence=sequence,
            frame_count=frame_count,
            payload_length=len(payload),
            azimuth_deg=azimuth_deg,
            elevation_deg=elevation_deg,
            directivity_blend_deg=blend,
            suppression_focus_active=focus_active,
            binaural_enabled=binaural_enabled,
            binaural_follow_steering=binaural_follow_steering,
            binaural_azimuth_deg=binaural_azimuth_deg,
            binaural_elevation_deg=binaural_elevation_deg,
            binaural_backend=backend_id(binaural_backend) if binaural_backend else BACKEND_NONE,
            suppression_ambient_floor_linear=sup.ambient_floor_linear,
            suppression_fade_ms=sup.fade_ms,
            suppression_activity_threshold=sup.activity_threshold,
            suppression_confidence_threshold=sup.confidence_threshold,
            suppression_envelope_attack_coeff=sup.envelope_attack_coeff,
            suppression_envelope_release_coeff=sup.envelope_release_coeff,
            suppression_confidence=sup.confidence,
        )
        try:
            self._process.stdin.write(header)
            self._process.stdin.write(payload)
            self._process.stdin.flush()
        except BrokenPipeError as exc:
            raise StreamProtocolError(f"broken pipe while sending block: {self.stderr_text()}") from exc
        self._last_sent_sequence = sequence
        self._next_sequence += 1
        return sequence

    def recv_block(self) -> tuple[np.ndarray, int]:
        """Receive one processed stereo block. Returns (pcm, sequence)."""
        if self._process.stdout is None:
            raise StreamProtocolError("subprocess stdout is closed")
        header_bytes = self._read_exact(OUTPUT_HEADER.size)
        header = unpack_output_header(header_bytes)
        assert_response_sequence(header.sequence, self._last_sent_sequence)
        payload = self._read_exact(header.payload_length)
        pcm = np.frombuffer(payload, dtype="<f4").reshape(header.frame_count, 2).copy()
        return pcm, header.sequence

    def _read_exact(self, num_bytes: int) -> bytes:
        assert self._process.stdout is not None
        buf = bytearray()
        while len(buf) < num_bytes:
            chunk = self._process.stdout.read(num_bytes - len(buf))
            if not chunk:
                raise StreamProtocolError(
                    f"sonitude_stream_process exited unexpectedly "
                    f"(returncode={self._process.poll()}): {self.stderr_text()}"
                )
            buf += chunk
        return bytes(buf)

    def close(self) -> None:
        try:
            if self._process.stdin and not self._process.stdin.closed:
                header = pack_input_header(
                    sequence=self._next_sequence,
                    frame_count=0,
                    payload_length=0,
                    azimuth_deg=0.0,
                    elevation_deg=0.0,
                    directivity_blend_deg=0.0,
                    suppression_focus_active=False,
                    message_type=MSG_SHUTDOWN,
                )
                self._process.stdin.write(header)
                self._process.stdin.flush()
                self._process.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            self._process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.terminate()

    def terminate(self) -> None:
        self._process.kill()
        try:
            self._process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
