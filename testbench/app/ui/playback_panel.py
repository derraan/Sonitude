"""RAW / PROCESSED / RESIDUAL stereo playback transport: play/pause/stop/seek/volume."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtMultimedia import QMediaPlayer
from PySide6.QtWidgets import (
    QButtonGroup,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QRadioButton,
    QSlider,
    QVBoxLayout,
    QWidget,
)

from app.audio_io.playback_engine import PlaybackEngine


def _format_ms(ms: int) -> str:
    total_seconds = max(0, ms) // 1000
    return f"{total_seconds // 60:02d}:{total_seconds % 60:02d}"


class PlaybackPanel(QWidget):
    sourceSelected = Signal(str)  # "raw" | "processed" | "residual"

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._engine = PlaybackEngine(self)
        self._sources: dict[str, Path] = {}
        self._seeking = False

        self._raw_radio = QRadioButton("Listening preview (ear-cup, not binaural)")
        self._processed_radio = QRadioButton("Final processed stereo")
        self._residual_radio = QRadioButton("DSP residual (not preview)")
        self._processed_radio.setChecked(True)
        self._source_group = QButtonGroup(self)
        for name, button in (
            ("raw", self._raw_radio),
            ("processed", self._processed_radio),
            ("residual", self._residual_radio),
        ):
            button.toggled.connect(lambda checked, n=name: checked and self._on_source_selected(n))
            self._source_group.addButton(button)

        self._play_button = QPushButton("Play")
        self._pause_button = QPushButton("Pause")
        self._stop_button = QPushButton("Stop")
        self._play_button.clicked.connect(self._engine.play)
        self._pause_button.clicked.connect(self._engine.pause)
        self._stop_button.clicked.connect(self._engine.stop)

        self._position_slider = QSlider(Qt.Orientation.Horizontal)
        self._position_slider.sliderPressed.connect(lambda: setattr(self, "_seeking", True))
        self._position_slider.sliderReleased.connect(self._on_seek_released)
        self._time_label = QLabel("00:00 / 00:00")

        self._volume_slider = QSlider(Qt.Orientation.Horizontal)
        self._volume_slider.setRange(0, 100)
        self._volume_slider.setValue(80)
        self._volume_slider.valueChanged.connect(lambda v: self._engine.set_volume(v / 100.0))
        self._engine.set_volume(0.8)

        self._engine.positionChanged.connect(self._on_position_changed)
        self._engine.durationChanged.connect(self._on_duration_changed)
        self._engine.errorOccurred.connect(lambda msg: self._time_label.setText(f"Playback error: {msg}"))

        source_row = QHBoxLayout()
        source_row.addWidget(self._raw_radio)
        source_row.addWidget(self._processed_radio)
        source_row.addWidget(self._residual_radio)
        source_row.addStretch(1)

        transport_row = QHBoxLayout()
        transport_row.addWidget(self._play_button)
        transport_row.addWidget(self._pause_button)
        transport_row.addWidget(self._stop_button)
        transport_row.addWidget(QLabel("Volume"))
        transport_row.addWidget(self._volume_slider)

        seek_row = QHBoxLayout()
        seek_row.addWidget(self._position_slider)
        seek_row.addWidget(self._time_label)

        layout = QVBoxLayout(self)
        layout.addLayout(source_row)
        layout.addLayout(transport_row)
        layout.addLayout(seek_row)

    def set_sources(self, *, raw: Path | None = None, processed: Path | None = None, residual: Path | None = None) -> None:
        """Point the panel at the stereo preview files for the current test result."""
        self._sources = {}
        if raw is not None:
            self._sources["raw"] = raw
        if processed is not None:
            self._sources["processed"] = processed
        if residual is not None:
            self._sources["residual"] = residual
        self._raw_radio.setEnabled(raw is not None)
        self._processed_radio.setEnabled(processed is not None)
        self._residual_radio.setEnabled(residual is not None)
        if self._source_group.checkedButton() is not None:
            self._on_source_selected(self._checked_source_name())

    def _checked_source_name(self) -> str:
        if self._raw_radio.isChecked():
            return "raw"
        if self._residual_radio.isChecked():
            return "residual"
        return "processed"

    def _on_source_selected(self, name: str) -> None:
        path = self._sources.get(name)
        if path is not None and path.exists():
            self._engine.stop()
            self._engine.load(path)
        self.sourceSelected.emit(name)

    def _on_position_changed(self, position_ms: int) -> None:
        if not self._seeking:
            self._position_slider.setValue(position_ms)
        self._time_label.setText(f"{_format_ms(position_ms)} / {_format_ms(self._engine.duration_ms())}")

    def _on_duration_changed(self, duration_ms: int) -> None:
        self._position_slider.setRange(0, max(0, duration_ms))

    def _on_seek_released(self) -> None:
        self._seeking = False
        self._engine.seek(self._position_slider.value())

    def stop(self) -> None:
        self._engine.stop()
