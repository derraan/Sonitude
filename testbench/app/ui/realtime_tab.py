"""Mode 2 — Real-Time tab: pick an input device, capture 6 mic channels,
stream them through sonitude_stream_process, hear the processed stereo
output live, and optionally save raw/processed recordings."""

from __future__ import annotations

import time

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSlider,
    QSpinBox,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from app.audio_io.block_queue import DEFAULT_CAPACITY
from app.audio_io.device_manager import InputDeviceInfo, list_input_devices
from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.controller.realtime_controller import RealtimeWorker
from app.processing.capabilities import query_tool_capabilities
from app.ui.binaural_controls import BinauralControls
from app.ui.layout_persist import KEY_REALTIME_H, REALTIME_H_DEFAULT, restore_splitter, save_splitter
from app.ui.level_meter import DbfsMeter
from app.ui.steering_controls import SteeringControls
from app.ui.suppressor_controls import SuppressorControls
from app.ui.visualization_panel import VisualizationPanel


def _compact_combo(combo: QComboBox) -> None:
    combo.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
    combo.setMinimumContentsLength(16)


def _scroll_area(inner: QWidget) -> QScrollArea:
    area = QScrollArea()
    area.setWidgetResizable(True)
    area.setFrameShape(QFrame.Shape.NoFrame)
    area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    area.setWidget(inner)
    return area


class RealtimeTab(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._config_path = DEFAULT_CONFIG_PATH
        self._worker: RealtimeWorker | None = None
        self._devices: list[InputDeviceInfo] = []
        self._active_sample_rate_hz: int = 44100
        self._capabilities = query_tool_capabilities("sonitude_stream_process")
        self._layout_restored = False
        self._last_plot_s = 0.0

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
        transport = QWidget()
        transport_row = QHBoxLayout(transport)
        transport_row.setContentsMargins(0, 0, 0, 0)
        transport_row.addWidget(self._start_btn)
        transport_row.addWidget(self._stop_btn)
        transport_row.addWidget(self._restart_btn)
        transport_row.addWidget(self._record_btn)

        self._device_combo = QComboBox()
        _compact_combo(self._device_combo)
        self._device_combo.currentIndexChanged.connect(self._on_device_changed)
        self._refresh_devices_btn = QPushButton("Refresh Devices")
        self._refresh_devices_btn.clicked.connect(self._refresh_devices)

        self._channels_label = QLabel("—")
        self._rate_label = QLabel("—")
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
        self._queue_status_label.setWordWrap(True)

        self._preamp_slider = QSlider(Qt.Orientation.Horizontal)
        self._preamp_slider.setRange(0, 40)
        self._preamp_slider.setValue(24)
        self._preamp_slider.setToolTip(
            "Common gain applied to all six mic channels before sonitude_stream_process."
        )
        self._preamp_label = QLabel("Preamp boost 24 dB")
        self._preamp_slider.valueChanged.connect(self._on_preamp_changed)

        device_box = QGroupBox("Device")
        device_form = QFormLayout(device_box)
        device_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        device_form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        device_picker = QWidget()
        device_row = QHBoxLayout(device_picker)
        device_row.setContentsMargins(0, 0, 0, 0)
        device_row.addWidget(self._device_combo, stretch=1)
        device_row.addWidget(self._refresh_devices_btn)
        device_form.addRow("Input device:", device_picker)
        device_form.addRow("Input channels:", self._channels_label)
        device_form.addRow("Sample rate:", self._rate_label)
        device_form.addRow("Buffer (frames):", self._block_size_spin)
        device_form.addRow("Capture queue capacity:", self._queue_capacity_spin)
        device_form.addRow(self._queue_status_label)
        preamp_row = QWidget()
        preamp_layout = QHBoxLayout(preamp_row)
        preamp_layout.setContentsMargins(0, 0, 0, 0)
        preamp_layout.addWidget(self._preamp_label)
        preamp_layout.addWidget(self._preamp_slider)
        device_form.addRow("Input gain:", preamp_row)

        self._steering = SteeringControls("Beamformer steering (delay-and-sum)")
        self._steering.azimuthChanged.connect(self._on_steering_changed)
        self._steering.blendChanged.connect(self._on_blend_changed)

        self._suppressor = SuppressorControls(self._capabilities)
        self._suppressor.changed.connect(self._push_suppressor)
        self._suppressor.backendChanged.connect(self._on_suppression_backend_changed)

        self._binaural = BinauralControls(self._capabilities)
        self._binaural.changed.connect(self._push_binaural)

        self._raw_meters = [DbfsMeter(str(i + 1)) for i in range(6)]
        self._processed_meters = [DbfsMeter("L"), DbfsMeter("R")]
        meters_box = QGroupBox("Levels")
        meters_layout = QVBoxLayout(meters_box)
        meters_layout.addWidget(QLabel("Raw input channels 1–6:"))
        for meter in self._raw_meters:
            meters_layout.addWidget(meter)
        meters_layout.addWidget(QLabel("Processed stereo output:"))
        for meter in self._processed_meters:
            meters_layout.addWidget(meter)

        config_inner = QWidget()
        config_layout = QVBoxLayout(config_inner)
        config_layout.addWidget(device_box)
        config_layout.addWidget(self._steering)
        config_layout.addWidget(self._suppressor)
        config_layout.addWidget(self._binaural)
        config_layout.addWidget(meters_box)
        config_layout.addStretch(1)

        self._save_raw_btn = QPushButton("Save Raw Recording")
        self._save_processed_btn = QPushButton("Save Processed Recording")
        self._save_raw_btn.clicked.connect(self._on_save_raw)
        self._save_processed_btn.clicked.connect(self._on_save_processed)
        self._save_raw_btn.setEnabled(False)
        self._save_processed_btn.setEnabled(False)
        save_row = QHBoxLayout()
        save_row.addWidget(self._save_raw_btn)
        save_row.addWidget(self._save_processed_btn)

        self._status_label = QLabel("Stopped")
        self._status_label.setWordWrap(True)

        footer = QWidget()
        footer_layout = QVBoxLayout(footer)
        footer_layout.setContentsMargins(0, 0, 0, 0)
        footer_layout.addLayout(save_row)
        footer_layout.addWidget(self._status_label)

        left_layout = QVBoxLayout()
        left_layout.addWidget(transport)
        left_layout.addWidget(_scroll_area(config_inner), stretch=1)
        left_layout.addWidget(footer)
        left_widget = QWidget()
        left_widget.setMinimumWidth(280)
        left_widget.setLayout(left_layout)

        self.visualization_panel = VisualizationPanel(show_spectrogram=False, show_levels=False)
        self.visualization_panel.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self.visualization_panel.setMinimumWidth(360)

        self._main_splitter = QSplitter(Qt.Orientation.Horizontal)
        self._main_splitter.setChildrenCollapsible(False)
        self._main_splitter.addWidget(left_widget)
        self._main_splitter.addWidget(self.visualization_panel)
        self._main_splitter.setStretchFactor(0, 38)
        self._main_splitter.setStretchFactor(1, 62)

        outer = QVBoxLayout(self)
        outer.addWidget(self._main_splitter)

        self._refresh_devices()

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        if not self._layout_restored:
            restore_splitter(self._main_splitter, KEY_REALTIME_H, REALTIME_H_DEFAULT)
            self._layout_restored = True

    def save_layout(self) -> None:
        save_splitter(self._main_splitter, KEY_REALTIME_H)

    def _push_binaural(self) -> None:
        if self._worker is not None:
            self._worker.set_binaural(self._binaural.request())

    def _on_suppression_backend_changed(self) -> None:
        if self._worker is not None and self._worker.isRunning():
            self._on_restart()

    def _push_suppressor(self) -> None:
        if self._worker is not None:
            self._worker.set_suppressor(self._suppressor.request())

    def _on_preamp_changed(self, value: int) -> None:
        self._preamp_label.setText(f"Preamp boost {value} dB")
        if self._worker is not None:
            self._worker.set_preamp_db(float(value))

    def _refresh_devices(self) -> None:
        self._devices = list_input_devices()
        self._device_combo.blockSignals(True)
        self._device_combo.clear()
        for device in self._devices:
            suffix = "" if device.max_input_channels >= 6 else "  (fewer than 6 channels)"
            self._device_combo.addItem(f"{device.name}{suffix}", userData=device.index)
            self._device_combo.setItemData(
                self._device_combo.count() - 1, device.name, Qt.ItemDataRole.ToolTipRole
            )
        self._device_combo.blockSignals(False)
        if self._devices:
            self._on_device_changed(self._device_combo.currentIndex())

    def _on_device_changed(self, index: int) -> None:
        if 0 <= index < len(self._devices):
            device = self._devices[index]
            self._channels_label.setText(str(device.max_input_channels))
            self._rate_label.setText(f"{int(device.default_sample_rate_hz)} Hz")

    def _on_steering_changed(self, azimuth_deg: float) -> None:
        if self._worker is not None:
            self._worker.set_steering(azimuth_deg, 0.0, self._steering.width_deg())

    def _on_blend_changed(self, width_deg: float) -> None:
        if self._worker is not None:
            self._worker.set_steering(self._steering.commanded_azimuth_deg(), 0.0, width_deg)

    def _reset_meters(self) -> None:
        for meter in (*self._raw_meters, *self._processed_meters):
            meter.reset_peak()

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
        self._reset_meters()
        worker = RealtimeWorker(
            device.index,
            self._config_path,
            config_summary.capture_sample_rate_hz,
            active_channel_map=config_summary.active_channel_map,
            block_size=self._block_size_spin.value(),
            suppression=self._suppressor.suppression_mode(),
            suppression_backend=self._suppressor.suppression_backend(),
            queue_capacity=self._queue_capacity_spin.value(),
            binaural=self._binaural.request(),
            suppressor=self._suppressor.request(),
        )
        worker.set_steering(self._steering.commanded_azimuth_deg(), 0.0, self._steering.width_deg())
        worker.set_preamp_db(float(self._preamp_slider.value()))
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
            meter.set_dbfs(float(level))
        for meter, level in zip(self._processed_meters, processed_levels):
            meter.set_dbfs(float(level))

    def _on_block_processed(self, raw_block, processed_block) -> None:
        now = time.monotonic()
        if now - self._last_plot_s < 0.05:
            return
        self._last_plot_s = now
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
