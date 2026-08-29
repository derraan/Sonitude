"""Live DSP preview of a recorded 6-channel file: stream_process + speaker out.

Steering and binaural are read every block so the recorded-data tab behaves
like a plugin: turn the dial, hear the beam move, without re-running batch.
"""

from __future__ import annotations

import logging
import threading
import time
from pathlib import Path

import numpy as np
import sounddevice as sd
import soundfile as sf
from PySide6.QtCore import QThread, Signal

from app.audio_io.playback_engine import monitor_gain_linear
from app.processing.stream_adapter import StreamProcessor, StreamProtocolError
from app.processing.suppression import SuppressionMode, parse_suppression_mode
from app.storage.models import BinauralRequest

logger = logging.getLogger(__name__)


class FilePreviewWorker(QThread):
    positionChanged = Signal(int)
    durationChanged = Signal(int)
    blockProcessed = Signal(object, object)
    errorOccurred = Signal(str)
    stopped = Signal()

    def __init__(
        self,
        input_path: str | Path,
        config_path: str | Path,
        sample_rate_hz: int,
        *,
        active_channel_map: list[int] | None = None,
        block_size: int = 1024,
        suppression: SuppressionMode | str = SuppressionMode.AUTO,
        binaural: BinauralRequest | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self._input_path = Path(input_path)
        self._config_path = Path(config_path)
        self._sample_rate_hz = sample_rate_hz
        self._block_size = block_size
        self._suppression = parse_suppression_mode(suppression)
        self._active_channel_map = list(active_channel_map) if active_channel_map else [0, 1, 2, 3, 4, 5]
        self._lock = threading.Lock()
        self._binaural = binaural or BinauralRequest()
        self._azimuth_deg = 0.0
        self._elevation_deg = 0.0
        self._width_deg = 0.0
        self._volume = 0.8
        self._boost_db = 24.0
        self._seek_frame: int | None = None
        self._stop = threading.Event()
        self._pause = threading.Event()
        self._frame_index = 0
        self._frames_total = 0

    def set_steering(self, azimuth_deg: float, elevation_deg: float = 0.0, width_deg: float = 0.0) -> None:
        with self._lock:
            self._azimuth_deg = azimuth_deg
            self._elevation_deg = elevation_deg
            self._width_deg = width_deg

    def set_binaural(self, request: BinauralRequest) -> None:
        with self._lock:
            self._binaural = request

    def set_volume(self, volume_0_to_1: float) -> None:
        with self._lock:
            self._volume = max(0.0, min(1.0, float(volume_0_to_1)))

    def set_boost_db(self, boost_db: float) -> None:
        with self._lock:
            self._boost_db = max(0.0, min(48.0, float(boost_db)))

    def seek_ms(self, position_ms: int) -> None:
        frame = int(max(0, position_ms) * self._sample_rate_hz / 1000)
        with self._lock:
            self._seek_frame = frame

    def pause(self) -> None:
        self._pause.set()

    def resume(self) -> None:
        self._pause.clear()

    def request_stop(self) -> None:
        self._stop.set()
        self._pause.clear()

    def duration_ms(self) -> int:
        if self._sample_rate_hz <= 0:
            return 0
        return int(self._frames_total * 1000 / self._sample_rate_hz)

    def run(self) -> None:
        processor: StreamProcessor | None = None
        output_stream: sd.OutputStream | None = None
        reader: sf.SoundFile | None = None
        last_plot = 0.0
        try:
            reader = sf.SoundFile(str(self._input_path))
            self._frames_total = len(reader)
            self.durationChanged.emit(self.duration_ms())
            required = max(self._active_channel_map) + 1
            if reader.channels < required:
                raise ValueError(
                    f"{self._input_path.name} has {reader.channels} channels; "
                    f"map needs {required}"
                )
            processor = StreamProcessor(
                self._config_path,
                sample_rate_hz=self._sample_rate_hz,
                max_block_frames=max(8192, self._block_size * 4),
                suppression=self._suppression,
            )
            output_stream = sd.OutputStream(
                channels=2,
                samplerate=self._sample_rate_hz,
                blocksize=self._block_size,
                dtype="float32",
            )
            output_stream.start()

            while not self._stop.is_set():
                while self._pause.is_set() and not self._stop.is_set():
                    time.sleep(0.02)
                with self._lock:
                    seek = self._seek_frame
                    self._seek_frame = None
                    azimuth = self._azimuth_deg
                    elevation = self._elevation_deg
                    width = self._width_deg
                    binaural = self._binaural
                    gain = monitor_gain_linear(self._volume, self._boost_db)
                if seek is not None:
                    reader.seek(min(max(0, seek), max(0, self._frames_total - 1)))
                    self._frame_index = reader.tell()
                block = reader.read(self._block_size, dtype="float32", always_2d=True)
                if len(block) == 0:
                    break
                raw = np.ascontiguousarray(block[:, self._active_channel_map], dtype=np.float32)
                bin_az = azimuth if binaural.follow_beamformer_steering else binaural.azimuth_deg
                bin_el = elevation if binaural.follow_beamformer_steering else binaural.elevation_deg
                processor.send_block(
                    raw,
                    azimuth,
                    elevation,
                    True,
                    width,
                    binaural_enabled=binaural.enabled,
                    binaural_follow_steering=binaural.follow_beamformer_steering,
                    binaural_azimuth_deg=bin_az,
                    binaural_elevation_deg=bin_el,
                    binaural_backend=binaural.backend,
                )
                processed, _seq = processor.recv_block()
                output_stream.write(np.clip(processed * gain, -1.0, 1.0).astype(np.float32))
                self._frame_index += len(block)
                self.positionChanged.emit(int(self._frame_index * 1000 / self._sample_rate_hz))
                now = time.monotonic()
                if now - last_plot >= 0.05:
                    last_plot = now
                    self.blockProcessed.emit(raw, processed)
        except (StreamProtocolError, sd.PortAudioError, OSError, ValueError, RuntimeError) as exc:
            logger.exception("Live DSP preview failed")
            self.errorOccurred.emit(str(exc))
        except Exception as exc:  # noqa: BLE001
            logger.exception("Unexpected live DSP exception")
            self.errorOccurred.emit(str(exc))
        finally:
            if output_stream is not None:
                try:
                    output_stream.stop()
                    output_stream.close()
                except Exception:
                    pass
            if processor is not None:
                try:
                    processor.close()
                except Exception:
                    processor.terminate()
            if reader is not None:
                reader.close()
            self.stopped.emit()
