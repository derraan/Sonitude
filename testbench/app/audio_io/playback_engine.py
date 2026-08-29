"""Stereo file playback: play/pause/stop/seek/volume over Qt Multimedia.

A thin wrapper so the UI layer doesn't touch QMediaPlayer directly. Playback
only ever operates on stereo WAV files (the processed output, the ear-cup RAW
preview, or a residual duplicated to stereo) — never on the raw 6-channel
file, which most audio backends can't render sensibly anyway.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QObject, QUrl, Signal
from PySide6.QtMultimedia import QAudioOutput, QMediaPlayer


class PlaybackEngine(QObject):
    positionChanged = Signal(int)  # milliseconds
    durationChanged = Signal(int)  # milliseconds
    playbackStateChanged = Signal(QMediaPlayer.PlaybackState)
    errorOccurred = Signal(str)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._player = QMediaPlayer(self)
        self._audio_output = QAudioOutput(self)
        self._player.setAudioOutput(self._audio_output)
        # Wrapped (not signal-to-signal connected) because QMediaPlayer's
        # position/duration signals carry a qlonglong, not int, in some
        # PySide6 builds, and direct connect() requires matching signatures.
        self._player.positionChanged.connect(lambda ms: self.positionChanged.emit(int(ms)))
        self._player.durationChanged.connect(lambda ms: self.durationChanged.emit(int(ms)))
        self._player.playbackStateChanged.connect(self.playbackStateChanged)
        self._player.errorOccurred.connect(
            lambda _err, description: self.errorOccurred.emit(description)
        )

    def load(self, path: str | Path) -> None:
        self._player.setSource(QUrl.fromLocalFile(str(path)))

    def play(self) -> None:
        self._player.play()

    def pause(self) -> None:
        self._player.pause()

    def stop(self) -> None:
        self._player.stop()

    def seek(self, position_ms: int) -> None:
        self._player.setPosition(position_ms)

    def set_volume(self, volume_0_to_1: float) -> None:
        self._audio_output.setVolume(max(0.0, min(1.0, volume_0_to_1)))

    def duration_ms(self) -> int:
        return self._player.duration()

    def position_ms(self) -> int:
        return self._player.position()
