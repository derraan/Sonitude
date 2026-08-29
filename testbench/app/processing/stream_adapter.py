"""Adapter around the ``sonitude_stream_process`` real-time streaming CLI tool.

Implements the binary framing protocol documented in
``src/tools/stream_process.cpp`` over a subprocess's stdin/stdout pipes. Like
``batch_adapter``, this module contains no DSP — it only frames/unframes PCM
blocks and drives the subprocess.
"""

from __future__ import annotations

import struct
import subprocess
from pathlib import Path

import numpy as np

from app.processing.sonitude_binary_locator import find_binary

_INPUT_MAGIC = 0x31424253  # "SBB1"
_OUTPUT_MAGIC = 0x314F4253  # "SBO1"
_INPUT_HEADER = struct.Struct("<IIfffB3x")  # magic, frame_count, az, el, width, flag, pad
_OUTPUT_HEADER = struct.Struct("<II")  # magic, frame_count
MIC_CHANNELS = 6
MAX_WIDTH_DEG = 180.0  # matches kMaxWidthDeg in the C++ tools


class StreamProtocolError(RuntimeError):
    """Raised on framing/magic mismatches or unexpected subprocess exit."""


class StreamProcessor:
    """Owns the sonitude_stream_process subprocess for one streaming session.

    Not thread-safe on its own; the caller (RealtimeWorker) is expected to
    call :meth:`send_block` / :meth:`recv_block` from a single dedicated
    thread, sequentially (write one block, then read its response) to avoid
    unbounded pipe buffering.
    """

    def __init__(
        self,
        config_path: str | Path,
        *,
        sample_rate_hz: int | None = None,
        max_block_frames: int = 8192,
        enable_suppression: bool = False,
        disable_limiter: bool = False,
        binary_path: str | Path | None = None,
        build_dir: str | Path | None = None,
    ) -> None:
        binary = Path(binary_path) if binary_path else find_binary("sonitude_stream_process", build_dir)
        command = [str(binary), "--config", str(config_path), "--max-block-frames", str(max_block_frames)]
        if sample_rate_hz is not None:
            command += ["--sample-rate", str(sample_rate_hz)]
        if enable_suppression:
            command.append("--enable-suppression")
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

    def send_block(
        self,
        mic_pcm: np.ndarray,
        azimuth_deg: float,
        elevation_deg: float,
        suppression_focus_active: bool,
        width_deg: float = 0.0,
    ) -> None:
        """Send one 6-channel float32 block, shape (frame_count, 6).

        ``width_deg`` (0-180) is the directivity-blend "beam width" defined in
        the C++ tools (see src/tools/stream_process.cpp's header comment) —
        0 is fully directional (the beamformer's own output, unchanged).
        """
        if self._process.stdin is None:
            raise StreamProtocolError("subprocess stdin is closed")
        frame_count = mic_pcm.shape[0]
        if mic_pcm.shape[1] != MIC_CHANNELS:
            raise ValueError(f"expected {MIC_CHANNELS} channels, got {mic_pcm.shape[1]}")
        clamped_width = max(0.0, min(MAX_WIDTH_DEG, float(width_deg)))
        header = _INPUT_HEADER.pack(
            _INPUT_MAGIC, frame_count, float(azimuth_deg), float(elevation_deg), clamped_width,
            1 if suppression_focus_active else 0,
        )
        payload = np.ascontiguousarray(mic_pcm, dtype="<f4").tobytes()
        self._process.stdin.write(header)
        self._process.stdin.write(payload)
        self._process.stdin.flush()

    def recv_block(self) -> np.ndarray:
        """Receive one processed stereo float32 block, shape (frame_count, 2)."""
        if self._process.stdout is None:
            raise StreamProtocolError("subprocess stdout is closed")
        header_bytes = self._read_exact(_OUTPUT_HEADER.size)
        magic, frame_count = _OUTPUT_HEADER.unpack(header_bytes)
        if magic != _OUTPUT_MAGIC:
            raise StreamProtocolError(f"bad output magic: {magic:#x}")
        payload = self._read_exact(frame_count * 2 * 4)
        return np.frombuffer(payload, dtype="<f4").reshape(frame_count, 2)

    def _read_exact(self, num_bytes: int) -> bytes:
        assert self._process.stdout is not None
        buf = bytearray()
        while len(buf) < num_bytes:
            chunk = self._process.stdout.read(num_bytes - len(buf))
            if not chunk:
                stderr = self._process.stderr.read().decode("utf-8", "replace") if self._process.stderr else ""
                raise StreamProtocolError(
                    f"sonitude_stream_process exited unexpectedly (returncode={self._process.poll()}): {stderr}"
                )
            buf += chunk
        return bytes(buf)

    def close(self) -> None:
        """Signal a clean shutdown (frame_count=0) and wait for the process to exit."""
        try:
            if self._process.stdin and not self._process.stdin.closed:
                self._process.stdin.write(_INPUT_HEADER.pack(_INPUT_MAGIC, 0, 0.0, 0.0, 0.0, 0))
                self._process.stdin.flush()
                self._process.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        self._process.wait(timeout=5)

    def terminate(self) -> None:
        """Hard-stop the subprocess (used on error paths)."""
        self._process.kill()
