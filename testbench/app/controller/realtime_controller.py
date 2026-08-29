"""Real-time (Mode 2) controller: captures 6-channel mic input, streams it
through sonitude_stream_process, and plays the processed stereo output —
all on a background QThread so the GUI never blocks on audio I/O.

Architected separately from BatchWorker per the real-time/streaming latency
and buffering constraints: this owns a live sounddevice input stream, a
live output stream, and the long-lived stream_process subprocess, rather
than a one-shot subprocess call per file.
"""

from __future__ import annotations

import logging
import queue
import threading
from pathlib import Path

import numpy as np
import sounddevice as sd
import soundfile as sf
from PySide6.QtCore import QThread, Signal

from app.processing.stream_adapter import StreamProcessor, StreamProtocolError

logger = logging.getLogger(__name__)

_EPS = 1e-12


def _rms_dbfs(block: np.ndarray) -> float:
    rms = float(np.sqrt(np.mean(np.square(block)) + _EPS))
    return 20.0 * np.log10(rms + _EPS)


class RealtimeWorker(QThread):
    levelsUpdated = Signal(list, list)  # raw_levels_dbfs[6], processed_levels_dbfs[2]
    blockProcessed = Signal(object, object)  # raw_block (frames,6), processed_block (frames,2)
    errorOccurred = Signal(str)
    started_ok = Signal()
    stopped = Signal()

    def __init__(
        self,
        input_device_index: int,
        config_path: str | Path,
        sample_rate_hz: int,
        *,
        block_size: int = 1024,
        output_device_index: int | None = None,
        enable_suppression: bool = False,
        disable_limiter: bool = False,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self._input_device_index = input_device_index
        self._output_device_index = output_device_index
        self._config_path = Path(config_path)
        self._sample_rate_hz = sample_rate_hz
        self._block_size = block_size
        self._enable_suppression = enable_suppression
        self._disable_limiter = disable_limiter

        self._stop_event = threading.Event()
        self._state_lock = threading.Lock()
        self._azimuth_deg = 0.0
        self._elevation_deg = 0.0
        self._suppression_focus_active = True

        self._recording = False
        self._raw_chunks: list[np.ndarray] = []
        self._processed_chunks: list[np.ndarray] = []

    # --- thread-safe setters called from the GUI thread ---------------------------------
    def set_steering(self, azimuth_deg: float, elevation_deg: float = 0.0) -> None:
        with self._state_lock:
            self._azimuth_deg = azimuth_deg
            self._elevation_deg = elevation_deg

    def set_suppression_focus(self, active: bool) -> None:
        with self._state_lock:
            self._suppression_focus_active = active

    def set_recording(self, recording: bool) -> None:
        with self._state_lock:
            self._recording = recording
            if recording:
                self._raw_chunks = []
                self._processed_chunks = []

    def request_stop(self) -> None:
        self._stop_event.set()

    def save_raw_recording(self, path: str | Path) -> None:
        if not self._raw_chunks:
            raise ValueError("no raw audio has been captured")
        sf.write(str(path), np.concatenate(self._raw_chunks, axis=0), self._sample_rate_hz)

    def save_processed_recording(self, path: str | Path) -> None:
        if not self._processed_chunks:
            raise ValueError("no processed audio has been captured")
        sf.write(str(path), np.concatenate(self._processed_chunks, axis=0), self._sample_rate_hz)

    # --- worker thread body ---------------------------------------------------------------
    def run(self) -> None:  # noqa: D102 - QThread entrypoint
        input_queue: queue.Queue[np.ndarray] = queue.Queue(maxsize=32)

        def input_callback(indata, frames, time_info, status) -> None:  # noqa: ANN001
            if status:
                logger.warning("input stream status: %s", status)
            try:
                input_queue.put_nowait(indata.copy())
            except queue.Full:
                pass  # drop this block rather than blocking the audio callback

        processor: StreamProcessor | None = None
        input_stream: sd.InputStream | None = None
        output_stream: sd.OutputStream | None = None
        try:
            processor = StreamProcessor(
                self._config_path,
                sample_rate_hz=self._sample_rate_hz,
                max_block_frames=max(8192, self._block_size * 4),
                enable_suppression=self._enable_suppression,
                disable_limiter=self._disable_limiter,
            )
            input_stream = sd.InputStream(
                device=self._input_device_index,
                channels=6,
                samplerate=self._sample_rate_hz,
                blocksize=self._block_size,
                dtype="float32",
                callback=input_callback,
            )
            output_stream = sd.OutputStream(
                device=self._output_device_index,
                channels=2,
                samplerate=self._sample_rate_hz,
                blocksize=self._block_size,
                dtype="float32",
            )
            input_stream.start()
            output_stream.start()
            self.started_ok.emit()

            while not self._stop_event.is_set():
                try:
                    raw_block = input_queue.get(timeout=0.5)
                except queue.Empty:
                    continue

                with self._state_lock:
                    azimuth = self._azimuth_deg
                    elevation = self._elevation_deg
                    focus_active = self._suppression_focus_active
                    recording = self._recording

                processor.send_block(raw_block, azimuth, elevation, focus_active)
                processed_block = processor.recv_block()

                output_stream.write(processed_block)

                if recording:
                    self._raw_chunks.append(raw_block.copy())
                    self._processed_chunks.append(processed_block.copy())

                raw_levels = [_rms_dbfs(raw_block[:, ch]) for ch in range(raw_block.shape[1])]
                processed_levels = [_rms_dbfs(processed_block[:, 0]), _rms_dbfs(processed_block[:, 1])]
                self.levelsUpdated.emit(raw_levels, processed_levels)
                self.blockProcessed.emit(raw_block, processed_block)

        except (StreamProtocolError, sd.PortAudioError, OSError) as exc:
            logger.exception("Real-time processing failed")
            self.errorOccurred.emit(str(exc))
        finally:
            if input_stream is not None:
                input_stream.stop()
                input_stream.close()
            if output_stream is not None:
                output_stream.stop()
                output_stream.close()
            if processor is not None:
                try:
                    processor.close()
                except Exception:  # noqa: BLE001 - best-effort cleanup
                    processor.terminate()
            self.stopped.emit()
