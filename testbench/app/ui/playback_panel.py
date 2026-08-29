"""Listen-to source selector plus play/pause/stop/seek/volume."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QButtonGroup,
    QGroupBox,
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
    livePlayRequested = Signal()
    livePauseRequested = Signal()
    liveStopRequested = Signal()
    liveSeekRequested = Signal(int)
    volumeChanged = Signal(float)
    boostChanged = Signal(float)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._engine = PlaybackEngine(self)
        self._sources: dict[str, Path] = {}
        self._seeking = False
        self._live = False

        listen_box = QGroupBox("Listen to")
        self._raw_radio = QRadioButton("Ear-cup preview")
        self._raw_radio.setToolTip("Uncalibrated ear-cup stereo. Not binaural output. Not used in residuals.")
        self._processed_radio = QRadioButton("Plot stage")
        self._processed_radio.setToolTip("The file currently selected in Plot stage.")
        self._residual_radio = QRadioButton("Plot residual")
        self._residual_radio.setToolTip("The DSP-tap residual currently selected in Plot residual.")
        self._processed_radio.setChecked(True)
        self._source_group = QButtonGroup(self)
        for name, button in (
            ("raw", self._raw_radio),
            ("processed", self._processed_radio),
            ("residual", self._residual_radio),
        ):
            button.toggled.connect(lambda checked, n=name: checked and self._on_source_selected(n))
            self._source_group.addButton(button)

        listen_row = QHBoxLayout(listen_box)
        listen_row.addWidget(self._raw_radio)
        listen_row.addWidget(self._processed_radio)
        listen_row.addWidget(self._residual_radio)
        listen_row.addStretch(1)

        self._play_button = QPushButton("Play")
        self._pause_button = QPushButton("Pause")
        self._stop_button = QPushButton("Stop")
        self._play_button.clicked.connect(self._on_play)
        self._pause_button.clicked.connect(self._on_pause)
        self._stop_button.clicked.connect(self._on_stop)

        self._position_slider = QSlider(Qt.Orientation.Horizontal)
        self._position_slider.sliderPressed.connect(lambda: setattr(self, "_seeking", True))
        self._position_slider.sliderReleased.connect(self._on_seek_released)
        self._time_label = QLabel("00:00 / 00:00")

        self._volume_slider = QSlider(Qt.Orientation.Horizontal)
        self._volume_slider.setRange(0, 100)
        self._volume_slider.setValue(80)
        self._volume_slider.valueChanged.connect(self._on_volume_changed)
        self._engine.set_volume(0.8)
        self._engine.set_boost_db(24.0)

        self._boost_slider = QSlider(Qt.Orientation.Horizontal)
        self._boost_slider.setRange(0, 40)
        self._boost_slider.setValue(24)
        self._boost_slider.setToolTip(
            "Monitor-only gain. This recording is far below 0 dBFS; unity-gain "
            "playback is inaudible. Does not change saved WAV files."
        )
        self._boost_slider.valueChanged.connect(self._on_boost_changed)
        self._boost_label = QLabel("Monitor boost 24 dB")

        self._engine.positionChanged.connect(self._on_position_changed)
        self._engine.durationChanged.connect(self._on_duration_changed)
        self._engine.errorOccurred.connect(lambda msg: self._time_label.setText(f"Playback error: {msg}"))

        transport_row = QHBoxLayout()
        transport_row.addWidget(self._play_button)
        transport_row.addWidget(self._pause_button)
        transport_row.addWidget(self._stop_button)
        transport_row.addWidget(QLabel("Volume"))
        transport_row.addWidget(self._volume_slider)
        transport_row.addWidget(self._boost_label)
        transport_row.addWidget(self._boost_slider)

        seek_row = QHBoxLayout()
        seek_row.addWidget(self._position_slider)
        seek_row.addWidget(self._time_label)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(listen_box)
        layout.addLayout(transport_row)
        layout.addLayout(seek_row)

    def listen_source(self) -> str:
        return self._checked_source_name()

    def set_live_mode(self, live: bool) -> None:
        self._live = bool(live)
        if live:
            self._engine.stop()

    def volume(self) -> float:
        return self._volume_slider.value() / 100.0

    def boost_db(self) -> float:
        return float(self._boost_slider.value())

    def set_clock(self, position_ms: int, duration_ms: int | None = None) -> None:
        if duration_ms is not None:
            self._position_slider.setRange(0, max(0, duration_ms))
            self._time_label.setText(f"{_format_ms(position_ms)} / {_format_ms(duration_ms)}")
        else:
            self._time_label.setText(f"{_format_ms(position_ms)} / {_format_ms(self._engine.duration_ms())}")
        if not self._seeking:
            self._position_slider.setValue(position_ms)

    def _on_volume_changed(self, value: int) -> None:
        self._engine.set_volume(value / 100.0)
        self.volumeChanged.emit(value / 100.0)

    def _on_boost_changed(self, value: int) -> None:
        self._boost_label.setText(f"Monitor boost {value} dB")
        self._engine.set_boost_db(value)
        self.boostChanged.emit(float(value))

    def _on_play(self) -> None:
        if self._live:
            self.livePlayRequested.emit()
            return
        self._engine.play()

    def _on_pause(self) -> None:
        if self._live:
            self.livePauseRequested.emit()
            return
        self._engine.pause()

    def _on_stop(self) -> None:
        if self._live:
            self.liveStopRequested.emit()
            return
        self._engine.stop()

    def set_sources(self, *, raw: Path | None = None, processed: Path | None = None, residual: Path | None = None) -> None:
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
        if not self._live:
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
        if self._live:
            self.liveSeekRequested.emit(self._position_slider.value())
            return
        self._engine.seek(self._position_slider.value())

    def stop(self) -> None:
        if self._live:
            self.liveStopRequested.emit()
            return
        self._engine.stop()
