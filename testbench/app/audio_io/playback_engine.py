"""Stereo file playback via PortAudio (sounddevice).

Qt Multimedia was advancing the playhead on this machine but the processed
capture sits around -60 dBFS, and QAudioOutput cannot apply gain above unity.
This engine streams PCM through the same default output device as live mode
and applies a monitor-only boost (clipped) so quiet recordings are audible.
"""

from __future__ import annotations

import threading
from pathlib import Path

import numpy as np
import sounddevice as sd
import soundfile as sf
from PySide6.QtCore import QObject, QTimer, Signal


def monitor_gain_linear(volume_0_to_1: float, boost_db: float) -> float:
    volume = max(0.0, min(1.0, float(volume_0_to_1)))
    boost_db = max(0.0, min(48.0, float(boost_db)))
    return volume * float(10.0 ** (boost_db / 20.0))


class PlaybackEngine(QObject):
    positionChanged = Signal(int)
    durationChanged = Signal(int)
    playbackStateChanged = Signal(str)
    errorOccurred = Signal(str)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._lock = threading.Lock()
        self._path: Path | None = None
        self._file: sf.SoundFile | None = None
        self._stream: sd.OutputStream | None = None
        self._sample_rate = 44100
        self._channels = 2
        self._frames_total = 0
        self._frame_index = 0
        self._volume = 0.8
        self._boost_db = 24.0
        self._playing = False
        self._timer = QTimer(self)
        self._timer.setInterval(50)
        self._timer.timeout.connect(self._emit_position)

    def load(self, path: str | Path) -> None:
        self.stop()
        path = Path(path)
        try:
            reader = sf.SoundFile(str(path))
        except Exception as exc:  # noqa: BLE001
            self.errorOccurred.emit(str(exc))
            return
        with self._lock:
            if self._file is not None:
                self._file.close()
            self._path = path
            self._file = reader
            self._sample_rate = int(reader.samplerate)
            self._channels = int(reader.channels)
            self._frames_total = len(reader)
            self._frame_index = 0
        self.durationChanged.emit(self.duration_ms())
        self.positionChanged.emit(0)

    def play(self) -> None:
        with self._lock:
            if self._file is None:
                self.errorOccurred.emit("No listen file loaded.")
                return
            if self._frame_index >= self._frames_total:
                self._frame_index = 0
            if self._playing and self._stream is not None:
                return
            try:
                self._file.seek(self._frame_index)
                self._stream = sd.OutputStream(
                    samplerate=self._sample_rate,
                    channels=2,
                    dtype="float32",
                    callback=self._callback,
                    blocksize=2048,
                )
                self._stream.start()
                self._playing = True
            except Exception as exc:  # noqa: BLE001
                self._playing = False
                self.errorOccurred.emit(f"Could not open speaker output: {exc}")
                return
        self._timer.start()
        self.playbackStateChanged.emit("playing")

    def pause(self) -> None:
        self._stop_stream(reset_position=False)
        self.playbackStateChanged.emit("paused")

    def stop(self) -> None:
        self._stop_stream(reset_position=True)
        self.playbackStateChanged.emit("stopped")
        self.positionChanged.emit(0)

    def seek(self, position_ms: int) -> None:
        frame = int(max(0, position_ms) * self._sample_rate / 1000)
        with self._lock:
            self._frame_index = min(frame, self._frames_total)
            if self._file is not None:
                try:
                    self._file.seek(self._frame_index)
                except Exception:
                    pass
        self.positionChanged.emit(self.position_ms())

    def set_volume(self, volume_0_to_1: float) -> None:
        with self._lock:
            self._volume = max(0.0, min(1.0, float(volume_0_to_1)))

    def set_boost_db(self, boost_db: float) -> None:
        with self._lock:
            self._boost_db = max(0.0, min(48.0, float(boost_db)))

    def duration_ms(self) -> int:
        if self._sample_rate <= 0:
            return 0
        return int(self._frames_total * 1000 / self._sample_rate)

    def position_ms(self) -> int:
        if self._sample_rate <= 0:
            return 0
        with self._lock:
            frame = self._frame_index
        return int(frame * 1000 / self._sample_rate)

    def _gain(self) -> float:
        return monitor_gain_linear(self._volume, self._boost_db)

    def _callback(self, outdata, frames, _time_info, _status) -> None:  # noqa: ANN001
        with self._lock:
            reader = self._file
            gain = self._gain()
            if reader is None or not self._playing:
                outdata.fill(0)
                return
            block = reader.read(frames, dtype="float32", always_2d=True)
            n = len(block)
            if n == 0:
                outdata.fill(0)
                self._playing = False
                return
            if block.shape[1] == 1:
                stereo = np.repeat(block, 2, axis=1)
            else:
                stereo = block[:, :2]
            boosted = np.clip(stereo * gain, -1.0, 1.0)
            outdata[:n, :2] = boosted
            if n < frames:
                outdata[n:] = 0
                self._playing = False
            self._frame_index += n

    def _stop_stream(self, *, reset_position: bool) -> None:
        self._timer.stop()
        with self._lock:
            self._playing = False
            stream = self._stream
            self._stream = None
            if reset_position:
                self._frame_index = 0
                if self._file is not None:
                    try:
                        self._file.seek(0)
                    except Exception:
                        pass
        if stream is not None:
            try:
                stream.abort()
            except Exception:
                pass
            try:
                stream.close()
            except Exception:
                pass

    def _emit_position(self) -> None:
        self.positionChanged.emit(self.position_ms())
        with self._lock:
            still = self._playing
        if not still:
            self._stop_stream(reset_position=False)
            self.playbackStateChanged.emit("stopped")
