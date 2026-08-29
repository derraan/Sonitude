"""Mode 2 — Real-Time tab: pick an input device, capture 6 mic channels,
stream them through sonitude_stream_process, hear the processed stereo
output live, and optionally save raw/processed recordings."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QSlider,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from app.audio_io.device_manager import InputDeviceInfo, list_input_devices
from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.controller.realtime_controller import RealtimeWorker
from app.ui.steering_dial import SteeringDial
from app.ui.visualization_panel import VisualizationPanel

_LEVEL_METER_MIN_DBFS = -60.0


def _level_to_percent(dbfs: float) -> int:
    return int(np.clip((dbfs - _LEVEL_METER_MIN_DBFS) / -_LEVEL_METER_MIN_DBFS * 100.0, 0, 100))


class RealtimeTab(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._config_path = DEFAULT_CONFIG_PATH
        self._worker: RealtimeWorker | None = None
        self._devices: list[InputDeviceInfo] = []
        self._active_sample_rate_hz: int = 44100

        self._device_combo = QComboBox()
        self._refresh_devices_btn = QPushButton("Refresh Devices")
        self._refresh_devices_btn.clicked.connect(self._refresh_devices)

        self._channels_label = QLabel("Input Channels: —")
        self._rate_label = QLabel("Sample Rate: —")
        self._block_size_spin = QSpinBox()
        self._block_size_spin.setRange(64, 8192)
        self._block_size_spin.setSingleStep(64)
        self._block_size_spin.setValue(1024)

        self._suppression_checkbox = QCheckBox("Enable Suppression")

        device_box = QGroupBox("Device")
        device_layout = QVBoxLayout(device_box)
        device_row = QHBoxLayout()
        device_row.addWidget(self._device_combo, stretch=1)
        device_row.addWidget(self._refresh_devices_btn)
        device_layout.addLayout(device_row)
        device_layout.addWidget(self._channels_label)
        device_layout.addWidget(self._rate_label)
        buffer_row = QHBoxLayout()
        buffer_row.addWidget(QLabel("Buffer (frames):"))
        buffer_row.addWidget(self._block_size_spin)
        device_layout.addLayout(buffer_row)
        device_layout.addWidget(self._suppression_checkbox)

        self._start_btn = QPushButton("START")
        self._stop_btn = QPushButton("STOP")
        self._record_btn = QPushButton("RECORD")
        self._record_btn.setCheckable(True)
        self._stop_btn.setEnabled(False)
        self._record_btn.setEnabled(False)
        self._start_btn.clicked.connect(self._on_start)
        self._stop_btn.clicked.connect(self._on_stop)
        self._record_btn.toggled.connect(self._on_record_toggled)
        transport_row = QHBoxLayout()
        transport_row.addWidget(self._start_btn)
        transport_row.addWidget(self._stop_btn)
        transport_row.addWidget(self._record_btn)

        self._save_raw_btn = QPushButton("Save Raw Recording")
        self._save_processed_btn = QPushButton("Save Processed Recording")
        self._save_raw_btn.clicked.connect(self._on_save_raw)
        self._save_processed_btn.clicked.connect(self._on_save_processed)
        self._save_raw_btn.setEnabled(False)
        self._save_processed_btn.setEnabled(False)
        save_row = QHBoxLayout()
        save_row.addWidget(self._save_raw_btn)
        save_row.addWidget(self._save_processed_btn)

        steering_box = QGroupBox("Steering (live)")
        self._steering_dial = SteeringDial()
        self._steering_readout = QLabel("0°")
        self._steering_dial.azimuthChanged.connect(self._on_steering_changed)
        self._width_slider = QSlider(Qt.Orientation.Horizontal)
        self._width_slider.setRange(0, 180)
        self._width_slider.setValue(0)
        self._width_label = QLabel("Width: 0° (fully directional)")
        self._width_slider.valueChanged.connect(self._on_width_changed)
        steering_layout = QVBoxLayout(steering_box)
        steering_layout.addWidget(self._steering_dial)
        steering_layout.addWidget(self._steering_readout, alignment=Qt.AlignmentFlag.AlignCenter)
        steering_layout.addWidget(self._width_label)
        steering_layout.addWidget(self._width_slider)

        self._raw_meters = [QProgressBar() for _ in range(6)]
        self._processed_meters = [QProgressBar() for _ in range(2)]
        meters_box = QGroupBox("Levels")
        meters_layout = QVBoxLayout(meters_box)
        meters_layout.addWidget(QLabel("Raw input channels 1-6:"))
        for meter in self._raw_meters:
            meter.setRange(0, 100)
            meters_layout.addWidget(meter)
        meters_layout.addWidget(QLabel("Processed stereo output (L, R):"))
        for meter in self._processed_meters:
            meter.setRange(0, 100)
            meters_layout.addWidget(meter)

        self._status_label = QLabel("Stopped")

        left_layout = QVBoxLayout()
        left_layout.addWidget(device_box)
        left_layout.addLayout(transport_row)
        left_layout.addLayout(save_row)
        left_layout.addWidget(steering_box)
        left_layout.addWidget(meters_box)
        left_layout.addWidget(self._status_label)
        left_layout.addStretch(1)
        left_widget = QWidget()
        left_widget.setLayout(left_layout)

        self.visualization_panel = VisualizationPanel()

        outer = QHBoxLayout(self)
        outer.addWidget(left_widget)
        outer.addWidget(self.visualization_panel, stretch=1)

        self._refresh_devices()
        try:
            self._suppression_checkbox.setChecked(read_runtime_config_summary(self._config_path).suppression_enabled)
        except Exception:  # noqa: BLE001 - default unchecked if config can't be read yet
            pass

    def _refresh_devices(self) -> None:
        self._devices = list_input_devices()
        self._device_combo.clear()
        for device in self._devices:
            suffix = "" if device.max_input_channels >= 6 else "  (fewer than 6 channels)"
            self._device_combo.addItem(f"{device.name}{suffix}", userData=device.index)
        if self._devices:
            self._on_device_changed(0)
        self._device_combo.currentIndexChanged.connect(self._on_device_changed)

    def _on_device_changed(self, index: int) -> None:
        if 0 <= index < len(self._devices):
            device = self._devices[index]
            self._channels_label.setText(f"Input Channels: {device.max_input_channels}")
            self._rate_label.setText(f"Sample Rate: {int(device.default_sample_rate_hz)} Hz")

    def _on_steering_changed(self, azimuth_deg: float) -> None:
        self._steering_readout.setText(f"{azimuth_deg:.0f}°")
        if self._worker is not None:
            self._worker.set_steering(azimuth_deg, 0.0, self._width_slider.value())

    def _on_width_changed(self, width_deg: int) -> None:
        descriptor = "fully directional" if width_deg == 0 else ("fully omnidirectional" if width_deg >= 180 else "blended")
        self._width_label.setText(f"Width: {width_deg}° ({descriptor})")
        self._steering_dial.set_width_deg(width_deg)
        if self._worker is not None:
            self._worker.set_steering(self._steering_dial.commanded_azimuth_deg(), 0.0, width_deg)

    def _on_start(self) -> None:
        index = self._device_combo.currentIndex()
        if index < 0 or index >= len(self._devices):
            QMessageBox.warning(self, "No device", "Select an input device first.")
            return
        device = self._devices[index]

        config_summary = read_runtime_config_summary(self._config_path)
        required_channels = max(config_summary.active_channel_map) + 1
        if device.max_input_channels < required_channels:
            QMessageBox.warning(
                self,
                "Not enough channels",
                f"config's active_channel_map {config_summary.active_channel_map} requires at least "
                f"{required_channels} device input channels, but the selected device only has "
                f"{device.max_input_channels}.",
            )
            return

        self._active_sample_rate_hz = config_summary.capture_sample_rate_hz
        worker = RealtimeWorker(
            device.index,
            self._config_path,
            config_summary.capture_sample_rate_hz,
            active_channel_map=config_summary.active_channel_map,
            block_size=self._block_size_spin.value(),
            enable_suppression=self._suppression_checkbox.isChecked(),
        )
        worker.set_steering(self._steering_dial.commanded_azimuth_deg(), 0.0, self._width_slider.value())
        worker.levelsUpdated.connect(self._on_levels_updated)
        worker.blockProcessed.connect(self._on_block_processed)
        worker.errorOccurred.connect(self._on_worker_error)
        worker.stopped.connect(self._on_worker_stopped)
        self._worker = worker
        worker.start()

        self._start_btn.setEnabled(False)
        self._stop_btn.setEnabled(True)
        self._record_btn.setEnabled(True)
        self._status_label.setText("Running")

    def _on_stop(self) -> None:
        if self._worker is not None:
            self._worker.request_stop()
        self._stop_btn.setEnabled(False)

    def _on_worker_stopped(self) -> None:
        self._start_btn.setEnabled(True)
        self._stop_btn.setEnabled(False)
        self._record_btn.setEnabled(False)
        self._record_btn.setChecked(False)
        self._status_label.setText("Stopped")

    def _on_worker_error(self, message: str) -> None:
        self._status_label.setText(f"Error: {message}")
        QMessageBox.critical(self, "Real-time processing error", message)

    def _on_record_toggled(self, checked: bool) -> None:
        if self._worker is not None:
            self._worker.set_recording(checked)
        self._save_raw_btn.setEnabled(not checked)
        self._save_processed_btn.setEnabled(not checked)
        self._status_label.setText("Recording" if checked else "Running")

    def _on_levels_updated(self, raw_levels: list, processed_levels: list) -> None:
        for meter, level in zip(self._raw_meters, raw_levels):
            meter.setValue(_level_to_percent(level))
        for meter, level in zip(self._processed_meters, processed_levels):
            meter.setValue(_level_to_percent(level))

    def _on_block_processed(self, raw_block, processed_block) -> None:
        self.visualization_panel.plot_waveforms(self._active_sample_rate_hz, raw=raw_block, processed=processed_block)

    def _on_save_raw(self) -> None:
        if self._worker is None:
            return
        path, _ = QFileDialog.getSaveFileName(self, "Save raw recording", filter="WAV files (*.wav)")
        if path:
            try:
                self._worker.save_raw_recording(path)
            except Exception as exc:  # noqa: BLE001
                QMessageBox.warning(self, "Save failed", str(exc))

    def _on_save_processed(self) -> None:
        if self._worker is None:
            return
        path, _ = QFileDialog.getSaveFileName(self, "Save processed recording", filter="WAV files (*.wav)")
        if path:
            try:
                self._worker.save_processed_recording(path)
            except Exception as exc:  # noqa: BLE001
                QMessageBox.warning(self, "Save failed", str(exc))
