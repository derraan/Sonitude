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
import tempfile
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
        active_channel_map: list[int] | None = None,
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

        # active_channel_map selects and reorders which raw DEVICE channels
        # are the six active mics (same field the C++ config uses for batch
        # mode, see config/default.yaml). A map like [2, 4, 6, 8, 10, 12]
        # means the device must supply at least 13 channels, and channel 2
        # (0-indexed) becomes mic 0, channel 4 becomes mic 1, and so on.
        # Without this, live mode silently associates the wrong device
        # channels with the array geometry/calibration whenever the map
        # isn't the identity [0, 1, 2, 3, 4, 5].
        self._active_channel_map = list(active_channel_map) if active_channel_map else [0, 1, 2, 3, 4, 5]
        self._device_channel_count = max(self._active_channel_map) + 1

        self._stop_event = threading.Event()
        self._state_lock = threading.Lock()
        self._azimuth_deg = 0.0
        self._elevation_deg = 0.0
        self._width_deg = 0.0
        self._suppression_focus_active = True

        # Recording streams straight to temp WAV files rather than
        # accumulating blocks in Python lists: at 44.1kHz/6ch float32 an
        # in-memory recording grows by roughly 1MB/s, which becomes several
        # GB over a long test session. Only file handles and small buffers
        # are held in memory regardless of recording length.
        self._recording_requested = False
        self._recording_active = False
        self._raw_recording_file: sf.SoundFile | None = None
        self._processed_recording_file: sf.SoundFile | None = None
        self._raw_temp_path: Path | None = None
        self._processed_temp_path: Path | None = None

    def device_channel_count(self) -> int:
        """Number of raw device channels that must be opened to satisfy active_channel_map."""
        return self._device_channel_count

    # --- thread-safe setters called from the GUI thread ---------------------------------
    def set_steering(self, azimuth_deg: float, elevation_deg: float = 0.0, width_deg: float = 0.0) -> None:
        with self._state_lock:
            self._azimuth_deg = azimuth_deg
            self._elevation_deg = elevation_deg
            self._width_deg = width_deg

    def set_suppression_focus(self, active: bool) -> None:
        with self._state_lock:
            self._suppression_focus_active = active

    def set_recording(self, recording: bool) -> None:
        # Only flip a flag here; the worker thread opens/closes the actual
        # SoundFile handles at the top of its next loop iteration, since
        # libsndfile handles aren't safe to open/close from a second thread
        # while the worker may be mid-write.
        with self._state_lock:
            self._recording_requested = recording

    def request_stop(self) -> None:
        self._stop_event.set()

    def save_raw_recording(self, path: str | Path) -> None:
        if self._raw_temp_path is None or not self._raw_temp_path.exists():
            raise ValueError("no raw audio has been captured")
        _copy_wav(self._raw_temp_path, Path(path))

    def save_processed_recording(self, path: str | Path) -> None:
        if self._processed_temp_path is None or not self._processed_temp_path.exists():
            raise ValueError("no processed audio has been captured")
        _copy_wav(self._processed_temp_path, Path(path))

    def _select_active_channels(self, device_block: np.ndarray) -> np.ndarray:
        """Apply active_channel_map: select/reorder device columns -> 6 active mics."""
        return device_block[:, self._active_channel_map]

    def _sync_recording_state(self) -> None:
        with self._state_lock:
            requested = self._recording_requested
        if requested and not self._recording_active:
            self._raw_temp_path = Path(tempfile.mktemp(suffix="_raw.wav"))
            self._processed_temp_path = Path(tempfile.mktemp(suffix="_processed.wav"))
            # subtype="FLOAT" is required: sf.SoundFile defaults to PCM_16
            # regardless of the array dtype passed to write() (unlike the
            # sf.write() convenience function, which infers FLOAT from a
            # float32 array) — without it, recordings were silently
            # quantized to 16-bit on every write.
            self._raw_recording_file = sf.SoundFile(
                str(self._raw_temp_path), mode="w", samplerate=self._sample_rate_hz, channels=6, subtype="FLOAT"
            )
            self._processed_recording_file = sf.SoundFile(
                str(self._processed_temp_path), mode="w", samplerate=self._sample_rate_hz, channels=2, subtype="FLOAT"
            )
            self._recording_active = True
        elif not requested and self._recording_active:
            self._close_recording_files()
            self._recording_active = False

    def _close_recording_files(self) -> None:
        if self._raw_recording_file is not None:
            self._raw_recording_file.close()
            self._raw_recording_file = None
        if self._processed_recording_file is not None:
            self._processed_recording_file.close()
            self._processed_recording_file = None

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
                channels=self._device_channel_count,
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
                self._sync_recording_state()
                try:
                    device_block = input_queue.get(timeout=0.5)
                except queue.Empty:
                    continue

                raw_block = self._select_active_channels(device_block)

                with self._state_lock:
                    azimuth = self._azimuth_deg
                    elevation = self._elevation_deg
                    width = self._width_deg
                    focus_active = self._suppression_focus_active

                processor.send_block(raw_block, azimuth, elevation, focus_active, width)
                processed_block = processor.recv_block()

                output_stream.write(processed_block)

                if self._recording_active:
                    self._raw_recording_file.write(raw_block)
                    self._processed_recording_file.write(processed_block)

                raw_levels = [_rms_dbfs(raw_block[:, ch]) for ch in range(raw_block.shape[1])]
                processed_levels = [_rms_dbfs(processed_block[:, 0]), _rms_dbfs(processed_block[:, 1])]
                self.levelsUpdated.emit(raw_levels, processed_levels)
                self.blockProcessed.emit(raw_block, processed_block)

        except (StreamProtocolError, sd.PortAudioError, OSError) as exc:
            logger.exception("Real-time processing failed")
            self.errorOccurred.emit(str(exc))
        finally:
            self._close_recording_files()
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


def _copy_wav(source: Path, dest: Path) -> None:
    import shutil

    shutil.copy2(str(source), str(dest))
