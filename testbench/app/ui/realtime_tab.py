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
    QDoubleSpinBox,
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

from app.audio_io.block_queue import DEFAULT_CAPACITY
from app.audio_io.device_manager import InputDeviceInfo, list_input_devices
from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.controller.realtime_controller import RealtimeWorker
from app.processing.capabilities import query_tool_capabilities
from app.processing.suppression import SuppressionMode
from app.storage.models import BinauralRequest
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
        self._capabilities = query_tool_capabilities("sonitude_stream_process")

        self._device_combo = QComboBox()
        self._device_combo.currentIndexChanged.connect(self._on_device_changed)
        self._refresh_devices_btn = QPushButton("Refresh Devices")
        self._refresh_devices_btn.clicked.connect(self._refresh_devices)

        self._channels_label = QLabel("Input Channels: —")
        self._rate_label = QLabel("Sample Rate: —")
        self._block_size_spin = QSpinBox()
        self._block_size_spin.setRange(64, 8192)
        self._block_size_spin.setSingleStep(64)
        self._block_size_spin.setValue(1024)

        self._queue_capacity_spin = QSpinBox()
        self._queue_capacity_spin.setRange(1, 16)
        self._queue_capacity_spin.setValue(DEFAULT_CAPACITY)
        self._queue_capacity_spin.setToolTip(
            "Bounded capture queue capacity. Newest-data-wins: when full, the oldest queued "
            "block is discarded. The default is a starting point, not a universally optimal size."
        )
        self._queue_status_label = QLabel("Queue: depth 0 / dropped 0 / overrun no")

        self._suppression_combo = QComboBox()
        self._suppression_combo.addItem("AUTO (use YAML)", userData=SuppressionMode.AUTO.value)
        self._suppression_combo.addItem("ON (force on)", userData=SuppressionMode.ON.value)
        self._suppression_combo.addItem("OFF (force off, overrides YAML)", userData=SuppressionMode.OFF.value)

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
        queue_row = QHBoxLayout()
        queue_row.addWidget(QLabel("Capture queue capacity:"))
        queue_row.addWidget(self._queue_capacity_spin)
        device_layout.addLayout(queue_row)
        device_layout.addWidget(self._queue_status_label)
        device_layout.addWidget(QLabel("Suppression:"))
        device_layout.addWidget(self._suppression_combo)

        self._start_btn = QPushButton("START")
        self._stop_btn = QPushButton("STOP")
        self._restart_btn = QPushButton("RESTART")
        self._record_btn = QPushButton("RECORD")
        self._record_btn.setCheckable(True)
        self._stop_btn.setEnabled(False)
        self._restart_btn.setEnabled(False)
        self._record_btn.setEnabled(False)
        self._start_btn.clicked.connect(self._on_start)
        self._stop_btn.clicked.connect(self._on_stop)
        self._restart_btn.clicked.connect(self._on_restart)
        self._record_btn.toggled.connect(self._on_record_toggled)
        transport_row = QHBoxLayout()
        transport_row.addWidget(self._start_btn)
        transport_row.addWidget(self._stop_btn)
        transport_row.addWidget(self._restart_btn)
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
        self._blend_slider = QSlider(Qt.Orientation.Horizontal)
        self._blend_slider.setRange(0, 180)
        self._blend_slider.setValue(0)
        self._blend_label = QLabel("Directional / Omni Blend: 0° (fully directional)")
        self._blend_slider.valueChanged.connect(self._on_blend_changed)
        steering_layout = QVBoxLayout(steering_box)
        steering_layout.addWidget(self._steering_dial)
        steering_layout.addWidget(self._steering_readout, alignment=Qt.AlignmentFlag.AlignCenter)
        steering_layout.addWidget(self._blend_label)
        steering_layout.addWidget(self._blend_slider)
        blend_note = QLabel("Mix toward the six-microphone average. Not measured physical beamwidth.")
        blend_note.setWordWrap(True)
        blend_note.setStyleSheet("color: #8a8a8a; font-size: 10px;")
        steering_layout.addWidget(blend_note)

        binaural_box = QGroupBox("Binaural renderer (capability-gated)")
        self._binaural_enable = QCheckBox("Binaural enabled")
        self._binaural_backend = QComboBox()
        self._binaural_follow = QCheckBox("Follow effective beamformer steering")
        self._binaural_follow.setChecked(True)
        self._binaural_az = QDoubleSpinBox()
        self._binaural_az.setRange(-180.0, 180.0)
        self._binaural_el = QDoubleSpinBox()
        self._binaural_el.setRange(-90.0, 90.0)
        self._binaural_note = QLabel("")
        self._binaural_note.setWordWrap(True)
        binaural_layout = QVBoxLayout(binaural_box)
        binaural_layout.addWidget(self._binaural_enable)
        binaural_layout.addWidget(self._binaural_backend)
        binaural_layout.addWidget(self._binaural_follow)
        bin_row = QHBoxLayout()
        bin_row.addWidget(QLabel("Az:"))
        bin_row.addWidget(self._binaural_az)
        bin_row.addWidget(QLabel("El:"))
        bin_row.addWidget(self._binaural_el)
        binaural_layout.addLayout(bin_row)
        binaural_layout.addWidget(self._binaural_note)
        self._populate_binaural_controls()
        self._binaural_enable.toggled.connect(self._push_binaural)
        self._binaural_backend.currentIndexChanged.connect(lambda _i: self._push_binaural())
        self._binaural_follow.toggled.connect(self._push_binaural)
        self._binaural_az.valueChanged.connect(lambda _v: self._push_binaural())
        self._binaural_el.valueChanged.connect(lambda _v: self._push_binaural())

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
        left_layout.addWidget(binaural_box)
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

    def _populate_binaural_controls(self) -> None:
        caps = self._capabilities.binaural
        self._binaural_backend.clear()
        usable = bool(caps.available and self._capabilities.queried and caps.backends)
        self._binaural_enable.setEnabled(usable)
        self._binaural_backend.setEnabled(usable)
        self._binaural_follow.setEnabled(usable)
        self._binaural_az.setEnabled(usable)
        self._binaural_el.setEnabled(usable)
        if not self._capabilities.queried:
            self._binaural_note.setText(
                "C++ binaural capabilities were not reported. Live capture still works without HRTF."
            )
            return
        for name in caps.backends:
            self._binaural_backend.addItem(name, userData=name)
        unavailable = ", ".join(caps.unavailable_backends) or "none"
        self._binaural_note.setText(f"{caps.note} Not offered: {unavailable}.")

    def _current_binaural(self) -> BinauralRequest:
        backend = self._binaural_backend.currentData()
        return BinauralRequest(
            enabled=self._binaural_enable.isChecked() and self._binaural_enable.isEnabled(),
            backend=backend if isinstance(backend, str) else None,
            azimuth_deg=self._binaural_az.value(),
            elevation_deg=self._binaural_el.value(),
            follow_beamformer_steering=self._binaural_follow.isChecked(),
        )

    def _push_binaural(self) -> None:
        if self._worker is not None:
            self._worker.set_binaural(self._current_binaural())

    def _refresh_devices(self) -> None:
        self._devices = list_input_devices()
        self._device_combo.blockSignals(True)
        self._device_combo.clear()
        for device in self._devices:
            suffix = "" if device.max_input_channels >= 6 else "  (fewer than 6 channels)"
            self._device_combo.addItem(f"{device.name}{suffix}", userData=device.index)
        self._device_combo.blockSignals(False)
        if self._devices:
            self._on_device_changed(self._device_combo.currentIndex())

    def _on_device_changed(self, index: int) -> None:
        if 0 <= index < len(self._devices):
            device = self._devices[index]
            self._channels_label.setText(f"Input Channels: {device.max_input_channels}")
            self._rate_label.setText(f"Sample Rate: {int(device.default_sample_rate_hz)} Hz")

    def _on_steering_changed(self, azimuth_deg: float) -> None:
        self._steering_readout.setText(f"{azimuth_deg:.0f}°")
        if self._worker is not None:
            self._worker.set_steering(azimuth_deg, 0.0, self._blend_slider.value())

    def _on_blend_changed(self, blend_deg: int) -> None:
        descriptor = "fully directional" if blend_deg == 0 else ("fully omnidirectional mix" if blend_deg >= 180 else "blended")
        self._blend_label.setText(f"Directional / Omni Blend: {blend_deg}° ({descriptor})")
        self._steering_dial.set_width_deg(blend_deg)
        if self._worker is not None:
            self._worker.set_steering(self._steering_dial.commanded_azimuth_deg(), 0.0, blend_deg)

    def _on_start(self) -> None:
        index = self._device_combo.currentIndex()
        if index < 0 or index >= len(self._devices):
            QMessageBox.warning(self, "No device", "Select an input device first.")
            return
        device = self._devices[index]

        try:
            config_summary = read_runtime_config_summary(self._config_path)
        except Exception as exc:  # noqa: BLE001
            QMessageBox.critical(self, "Malformed configuration", str(exc))
            return
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
            suppression=self._suppression_combo.currentData() or SuppressionMode.AUTO.value,
            queue_capacity=self._queue_capacity_spin.value(),
            binaural=self._current_binaural(),
        )
        worker.set_steering(self._steering_dial.commanded_azimuth_deg(), 0.0, self._blend_slider.value())
        worker.levelsUpdated.connect(self._on_levels_updated)
        worker.blockProcessed.connect(self._on_block_processed)
        worker.queueStatus.connect(self._on_queue_status)
        worker.errorOccurred.connect(self._on_worker_error)
        worker.stopped.connect(self._on_worker_stopped)
        self._worker = worker
        worker.start()

        self._start_btn.setEnabled(False)
        self._stop_btn.setEnabled(True)
        self._restart_btn.setEnabled(True)
        self._record_btn.setEnabled(True)
        self._status_label.setText("Running")

    def _on_stop(self) -> None:
        if self._worker is not None:
            self._worker.request_stop()
        self._stop_btn.setEnabled(False)

    def _on_restart(self) -> None:
        if self._worker is not None and self._worker.isRunning():
            self._worker.stopped.connect(self._restart_after_stop)
            self._worker.request_stop()
            return
        self._on_start()

    def _restart_after_stop(self) -> None:
        sender = self.sender()
        if sender is not None:
            try:
                sender.stopped.disconnect(self._restart_after_stop)
            except (RuntimeError, TypeError):
                pass
        self._on_start()

    def _on_worker_stopped(self) -> None:
        self._start_btn.setEnabled(True)
        self._stop_btn.setEnabled(False)
        self._restart_btn.setEnabled(True)
        self._record_btn.setEnabled(False)
        self._record_btn.setChecked(False)
        if "Error" not in self._status_label.text():
            self._status_label.setText("Stopped")

    def _on_worker_error(self, message: str) -> None:
        self._status_label.setText(f"Error: {message}")
        QMessageBox.critical(self, "Real-time processing error", message)

    def _on_queue_status(self, depth: int, dropped: int, overrun: bool) -> None:
        self._queue_status_label.setText(
            f"Queue: depth {depth} / dropped {dropped} / overrun {'yes' if overrun else 'no'}"
        )

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
