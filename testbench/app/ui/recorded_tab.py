"""Mode 1 — Recorded Data tab: select WAV file(s)/folder, inspect metadata,
run the existing algorithm via BatchWorker, and inspect results."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QSlider,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from app.audio_io import wav_loader
from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.controller.batch_controller import BatchWorker
from app.storage.models import SteeringEvent
from app.storage.result_store import ResultStore
from app.ui.metrics_panel import MetricsPanel
from app.ui.playback_panel import PlaybackPanel
from app.ui.steering_dial import SteeringDial
from app.ui.visualization_panel import VisualizationPanel

_METADATA_FIELDS = ["filename", "sample_rate_hz", "channels", "bit_depth", "duration_s", "num_samples"]


class RecordedDataTab(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._config_path = DEFAULT_CONFIG_PATH
        self._selected_paths: list[Path] = []
        self._result_store = ResultStore()
        self._batch_worker: BatchWorker | None = None
        self._last_metrics: dict[str, dict] = {}

        # --- left column: selection, metadata, batch controls -----------------------------
        select_row = QHBoxLayout()
        select_file_btn = QPushButton("Select WAV")
        select_folder_btn = QPushButton("Select Folder")
        select_file_btn.clicked.connect(self._on_select_file)
        select_folder_btn.clicked.connect(self._on_select_folder)
        select_row.addWidget(select_file_btn)
        select_row.addWidget(select_folder_btn)

        self._file_list = QListWidget()
        self._file_list.currentRowChanged.connect(self._on_file_row_changed)

        self._metadata_table = QTableWidget(len(_METADATA_FIELDS), 1)
        self._metadata_table.setVerticalHeaderLabels(_METADATA_FIELDS)
        self._metadata_table.horizontalHeader().setVisible(False)
        self._metadata_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._validation_label = QLabel("")
        self._validation_label.setWordWrap(True)

        steering_box = QGroupBox("Steering (single commanded direction for this batch)")
        self._steering_dial = SteeringDial()
        self._steering_readout = QLabel("0°")
        self._steering_dial.azimuthChanged.connect(lambda az: self._steering_readout.setText(f"{az:.0f}°"))
        self._width_slider = QSlider(Qt.Orientation.Horizontal)
        self._width_slider.setRange(0, 180)
        self._width_label = QLabel("Width: 0° (fully directional)")
        self._width_slider.valueChanged.connect(self._on_width_changed)
        steering_layout = QVBoxLayout(steering_box)
        steering_layout.addWidget(self._steering_dial)
        steering_layout.addWidget(self._steering_readout, alignment=Qt.AlignmentFlag.AlignCenter)
        steering_layout.addWidget(self._width_label)
        steering_layout.addWidget(self._width_slider)

        suppression_box = QGroupBox("Suppression")
        self._suppression_checkbox = QCheckBox("Enable Suppression")
        suppression_layout = QVBoxLayout(suppression_box)
        suppression_layout.addWidget(self._suppression_checkbox)

        steering_test_box = QGroupBox("Objective Steering Test (optional, slower)")
        self._steering_test_checkbox = QCheckBox("Run steering sweep for this batch")
        self._expected_azimuth_spin = QDoubleSpinBox()
        self._expected_azimuth_spin.setRange(-180.0, 180.0)
        self._expected_azimuth_spin.setSuffix("°")
        self._expected_azimuth_spin.setToolTip(
            "Known/expected direction of the dominant source in the recording, used as ground "
            "truth to score the measured beam-response-peak sweep."
        )
        steering_test_layout = QVBoxLayout(steering_test_box)
        steering_test_layout.addWidget(self._steering_test_checkbox)
        expected_row = QHBoxLayout()
        expected_row.addWidget(QLabel("Expected source direction:"))
        expected_row.addWidget(self._expected_azimuth_spin)
        steering_test_layout.addLayout(expected_row)

        self._process_selected_btn = QPushButton("Process Selected")
        self._process_batch_btn = QPushButton("Process Batch")
        self._process_selected_btn.clicked.connect(self._on_process_selected)
        self._process_batch_btn.clicked.connect(self._on_process_batch)
        process_row = QHBoxLayout()
        process_row.addWidget(self._process_selected_btn)
        process_row.addWidget(self._process_batch_btn)

        self._progress_bar = QProgressBar()
        self._status_label = QLabel("Idle")

        left_layout = QVBoxLayout()
        left_layout.addLayout(select_row)
        left_layout.addWidget(self._file_list)
        left_layout.addWidget(QLabel("Input information:"))
        left_layout.addWidget(self._metadata_table)
        left_layout.addWidget(self._validation_label)
        left_layout.addWidget(steering_box)
        left_layout.addWidget(suppression_box)
        left_layout.addWidget(steering_test_box)
        left_layout.addLayout(process_row)
        left_layout.addWidget(self._progress_bar)
        left_layout.addWidget(self._status_label)
        left_widget = QWidget()
        left_widget.setLayout(left_layout)

        # --- right column: results (select which processed test to inspect) --------------
        self._results_combo = QComboBox()
        self._results_combo.currentTextChanged.connect(self._on_result_selected)
        self._residual_stage_combo = QComboBox()
        self._residual_stage_combo.addItem("Beamform stage (beamformed − suppressed)", userData="residual_beamform.wav")
        self._residual_stage_combo.addItem("Limiter stage (suppressed − processed)", userData="residual_limiter.wav")
        self._residual_stage_combo.currentIndexChanged.connect(lambda _i: self._on_result_selected(self._results_combo.currentText()))
        domain_note = QLabel(
            "Note: RAW is an uncalibrated ear-cup listening preview, not the algorithm's input — it "
            "is never subtracted from PROCESSED. RESIDUAL is a stage-to-stage difference within the "
            "algorithm (see testbench/README.md, \"Residual definition\")."
        )
        domain_note.setWordWrap(True)
        domain_note.setStyleSheet("color: #8a8a8a; font-size: 10px;")
        self.playback_panel = PlaybackPanel()
        self.visualization_panel = VisualizationPanel()
        self.metrics_panel = MetricsPanel()

        right_layout = QVBoxLayout()
        right_layout.addWidget(QLabel("Result:"))
        right_layout.addWidget(self._results_combo)
        right_layout.addWidget(QLabel("Residual stage:"))
        right_layout.addWidget(self._residual_stage_combo)
        right_layout.addWidget(domain_note)
        right_layout.addWidget(self.playback_panel)
        right_layout.addWidget(self.visualization_panel, stretch=1)
        right_layout.addWidget(self.metrics_panel)
        right_widget = QWidget()
        right_widget.setLayout(right_layout)

        splitter = QSplitter()
        splitter.addWidget(left_widget)
        splitter.addWidget(right_widget)
        splitter.setStretchFactor(1, 1)

        outer = QVBoxLayout(self)
        outer.addWidget(splitter)

        try:
            self._suppression_checkbox.setChecked(read_runtime_config_summary(self._config_path).suppression_enabled)
        except Exception:  # noqa: BLE001 - default unchecked if config can't be read yet
            pass

    def _on_width_changed(self, width_deg: int) -> None:
        descriptor = "fully directional" if width_deg == 0 else ("fully omnidirectional" if width_deg >= 180 else "blended")
        self._width_label.setText(f"Width: {width_deg}° ({descriptor})")
        self._steering_dial.set_width_deg(width_deg)

    # --- file selection --------------------------------------------------------------------
    def _on_select_file(self) -> None:
        paths, _ = QFileDialog.getOpenFileNames(self, "Select 6-channel WAV file(s)", filter="WAV files (*.wav)")
        if paths:
            self._set_selected_paths([Path(p) for p in paths])

    def _on_select_folder(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Select folder of WAV files")
        if folder:
            self._set_selected_paths(wav_loader.find_wav_files(folder))

    def _set_selected_paths(self, paths: list[Path]) -> None:
        self._selected_paths = paths
        self._file_list.clear()
        self._file_list.addItems(str(p) for p in paths)
        if paths:
            self._file_list.setCurrentRow(0)

    def _on_file_row_changed(self, row: int) -> None:
        if row < 0 or row >= len(self._selected_paths):
            return
        self._show_metadata(self._selected_paths[row])

    def _show_metadata(self, path: Path) -> None:
        try:
            config_summary = read_runtime_config_summary(self._config_path)
            validation = wav_loader.validate_six_channel_wav(path, config_summary.capture_sample_rate_hz)
        except Exception as exc:  # noqa: BLE001
            self._validation_label.setText(f"Could not read config/file: {exc}")
            return

        if validation.metadata is not None:
            data = validation.metadata.as_dict()
            for row, field in enumerate(_METADATA_FIELDS):
                self._metadata_table.setItem(row, 0, QTableWidgetItem(str(data.get(field, ""))))

        if validation.ok:
            self._validation_label.setStyleSheet("color: #4caf50;")
            self._validation_label.setText("Valid: ready to process.")
        else:
            self._validation_label.setStyleSheet("color: #e05a5a;")
            self._validation_label.setText("Invalid: " + "; ".join(validation.errors))

    # --- processing --------------------------------------------------------------------------
    def _on_process_selected(self) -> None:
        row = self._file_list.currentRow()
        if row < 0:
            QMessageBox.warning(self, "No file selected", "Select a file first.")
            return
        self._run_batch([self._selected_paths[row]])

    def _on_process_batch(self) -> None:
        if not self._selected_paths:
            QMessageBox.warning(self, "No files selected", "Select a file or folder first.")
            return
        self._run_batch(self._selected_paths)

    def _run_batch(self, paths: list[Path]) -> None:
        if self._batch_worker is not None and self._batch_worker.isRunning():
            QMessageBox.information(self, "Busy", "A batch is already processing.")
            return

        steering_events = [
            SteeringEvent(
                0.0,
                self._steering_dial.commanded_azimuth_deg(),
                0.0,
                width_deg=self._width_slider.value(),
            )
        ]
        self._progress_bar.setRange(0, len(paths))
        self._progress_bar.setValue(0)
        self._status_label.setText(f"Processing 0/{len(paths)}...")

        expected_azimuth = (
            self._expected_azimuth_spin.value() if self._steering_test_checkbox.isChecked() else None
        )
        worker = BatchWorker(
            paths,
            self._config_path,
            steering_events,
            enable_suppression=self._suppression_checkbox.isChecked(),
            steering_test_expected_azimuth_deg=expected_azimuth,
            result_store=self._result_store,
        )
        worker.progress.connect(lambda done, total: self._progress_bar.setValue(done))
        worker.file_finished.connect(self._on_file_finished)
        worker.file_failed.connect(self._on_file_failed)
        worker.finished_all.connect(lambda: self._status_label.setText("Done."))
        self._batch_worker = worker
        worker.start()

    def _on_file_finished(self, input_path: str, result: dict) -> None:
        test_id = result["test_id"]
        self._last_metrics[test_id] = result["metrics"]
        self._results_combo.addItem(test_id)
        self._results_combo.setCurrentText(test_id)
        self._status_label.setText(f"Finished: {Path(input_path).name} -> {test_id}")

    def _on_file_failed(self, input_path: str, error: str) -> None:
        self._status_label.setText(f"Failed: {Path(input_path).name}: {error}")

    def _on_result_selected(self, test_id: str) -> None:
        if not test_id:
            return
        test_root = self._result_store.results_dir / test_id
        residual_filename = self._residual_stage_combo.currentData() or "residual_beamform.wav"
        self.playback_panel.set_sources(
            raw=test_root / "raw_preview_stereo.wav",
            processed=test_root / "processed_stereo.wav",
            residual=test_root / residual_filename,
        )
        metrics = self._last_metrics.get(test_id) or self._result_store.load_metrics(test_id)
        self.metrics_panel.update_metrics(metrics)

        sweep = metrics.get("steering", {}).get("objective_sweep_test")
        self._steering_dial.set_estimated_azimuth_deg(sweep["measured_peak_azimuth_deg"] if sweep else None)

        try:
            processed_data, sample_rate = wav_loader.load_wav(test_root / "processed_stereo.wav")
            raw_data, _ = wav_loader.load_wav(test_root / "raw_preview_stereo.wav")
            residual_data, _ = wav_loader.load_wav(test_root / residual_filename)
            self.visualization_panel.plot_waveforms(sample_rate, raw=raw_data, processed=processed_data, residual=residual_data)
            self.visualization_panel.plot_spectrogram(processed_data, sample_rate)
            self.visualization_panel.plot_levels(processed_data, sample_rate)
        except Exception:  # noqa: BLE001 - visualization is best-effort
            pass
