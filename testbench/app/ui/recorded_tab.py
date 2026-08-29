"""Mode 1 — Recorded Data tab: select audio file(s)/folder, inspect metadata,
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

from app.audio_io import audio_loader
from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.controller.batch_controller import BatchWorker
from app.processing.capabilities import query_tool_capabilities
from app.processing.suppression import SuppressionMode
from app.storage.models import BinauralRequest, SteeringEvent
from app.storage.result_store import ResultStore
from app.ui.metrics_panel import MetricsPanel
from app.ui.playback_panel import PlaybackPanel
from app.ui.steering_dial import SteeringDial
from app.ui.visualization_panel import VisualizationPanel

_METADATA_FIELDS = [
    "filename",
    "container",
    "sample_rate_hz",
    "channels",
    "bit_depth",
    "duration_s",
    "num_samples",
]
_AUDIO_FILTER = "Audio files (*.wav *.flac *.mp3);;WAV (*.wav);;FLAC (*.flac);;MP3 (*.mp3)"


class RecordedDataTab(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._config_path = DEFAULT_CONFIG_PATH
        self._selected_paths: list[Path] = []
        self._result_store = ResultStore()
        self._batch_worker: BatchWorker | None = None
        self._last_metrics: dict[str, dict] = {}
        self._capabilities = query_tool_capabilities("sonitude_wav_replay")

        select_row = QHBoxLayout()
        select_file_btn = QPushButton("Select Audio")
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
        self._blend_slider = QSlider(Qt.Orientation.Horizontal)
        self._blend_slider.setRange(0, 180)
        self._blend_label = QLabel("Directional / Omni Blend: 0° (fully directional)")
        self._blend_slider.valueChanged.connect(self._on_blend_changed)
        steering_layout = QVBoxLayout(steering_box)
        steering_layout.addWidget(self._steering_dial)
        steering_layout.addWidget(self._steering_readout, alignment=Qt.AlignmentFlag.AlignCenter)
        steering_layout.addWidget(self._blend_label)
        steering_layout.addWidget(self._blend_slider)
        blend_note = QLabel("This mixes beamformed audio toward the six-microphone average. It is not measured beamwidth / HPBW.")
        blend_note.setWordWrap(True)
        blend_note.setStyleSheet("color: #8a8a8a; font-size: 10px;")
        steering_layout.addWidget(blend_note)

        suppression_box = QGroupBox("Suppression (requested vs YAML)")
        self._suppression_combo = QComboBox()
        self._suppression_combo.addItem("AUTO (use YAML)", userData=SuppressionMode.AUTO.value)
        self._suppression_combo.addItem("ON (force on)", userData=SuppressionMode.ON.value)
        self._suppression_combo.addItem("OFF (force off, overrides YAML)", userData=SuppressionMode.OFF.value)
        suppression_layout = QVBoxLayout(suppression_box)
        suppression_layout.addWidget(self._suppression_combo)

        export_box = QGroupBox("Final result export")
        self._export_combo = QComboBox()
        self._export_combo.addItem("WAV (float32)", userData="wav")
        self._export_combo.addItem("FLAC (PCM_24 lossless integer)", userData="flac")
        export_note = QLabel("Export container does not change DSP processing. Intermediates stay WAV.")
        export_note.setWordWrap(True)
        export_layout = QVBoxLayout(export_box)
        export_layout.addWidget(self._export_combo)
        export_layout.addWidget(export_note)

        binaural_box = QGroupBox("Binaural renderer (C++ backend, capability-gated)")
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
        binaural_layout.addWidget(QLabel("Renderer backend:"))
        binaural_layout.addWidget(self._binaural_backend)
        binaural_layout.addWidget(self._binaural_follow)
        az_row = QHBoxLayout()
        az_row.addWidget(QLabel("Azimuth:"))
        az_row.addWidget(self._binaural_az)
        az_row.addWidget(QLabel("Elevation:"))
        az_row.addWidget(self._binaural_el)
        binaural_layout.addLayout(az_row)
        binaural_layout.addWidget(self._binaural_note)
        self._populate_binaural_controls()

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
        left_layout.addWidget(export_box)
        left_layout.addWidget(binaural_box)
        left_layout.addWidget(steering_test_box)
        left_layout.addLayout(process_row)
        left_layout.addWidget(self._progress_bar)
        left_layout.addWidget(self._status_label)
        left_widget = QWidget()
        left_widget.setLayout(left_layout)

        self._results_combo = QComboBox()
        self._results_combo.currentTextChanged.connect(self._on_result_selected)
        self._stage_combo = QComboBox()
        self._stage_combo.addItem(
            "Listening preview (ear-cup stereo — not binaural, never used in residuals)",
            userData="raw_preview_stereo.wav",
        )
        self._stage_combo.addItem("Beamformed (pre-suppression mono, stereo-duplicated for listen)", userData="beamformed.wav")
        self._stage_combo.addItem("Suppressed (pre-limiter mono)", userData="suppressed.wav")
        self._stage_combo.addItem("Binaural stereo (C++ tap if present; not the ear-cup preview)", userData="binaural_stereo.wav")
        self._stage_combo.addItem("Final processed stereo", userData="processed_stereo.wav")
        self._stage_combo.currentIndexChanged.connect(lambda _i: self._on_result_selected(self._results_combo.currentText()))
        self._residual_stage_combo = QComboBox()
        self._residual_stage_combo.addItem("Beamform stage (beamformed − suppressed)", userData="residual_beamform.wav")
        self._residual_stage_combo.addItem("Limiter stage (suppressed − processed)", userData="residual_limiter.wav")
        self._residual_stage_combo.currentIndexChanged.connect(lambda _i: self._on_result_selected(self._results_combo.currentText()))
        domain_note = QLabel(
            "The ear-cup stereo file is a listening-only preview. It is never binaural output and is "
            "never subtracted for residual metrics. DSP stages (beamformed / suppressed / binaural / "
            "final) come from the C++ taps."
        )
        domain_note.setWordWrap(True)
        domain_note.setStyleSheet("color: #8a8a8a; font-size: 10px;")
        self.playback_panel = PlaybackPanel()
        self.visualization_panel = VisualizationPanel()
        self.metrics_panel = MetricsPanel()

        right_layout = QVBoxLayout()
        right_layout.addWidget(QLabel("Result:"))
        right_layout.addWidget(self._results_combo)
        right_layout.addWidget(QLabel("DSP / listening stage:"))
        right_layout.addWidget(self._stage_combo)
        right_layout.addWidget(QLabel("Residual stage (DSP taps only):"))
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
            if read_runtime_config_summary(self._config_path).suppression_enabled:
                self._suppression_combo.setCurrentIndex(0)
        except Exception:  # noqa: BLE001 - default AUTO if config can't be read yet
            pass

    def _populate_binaural_controls(self) -> None:
        caps = self._capabilities.binaural
        self._binaural_backend.clear()
        if not caps.available or not self._capabilities.queried:
            self._binaural_enable.setEnabled(False)
            self._binaural_backend.setEnabled(False)
            self._binaural_follow.setEnabled(False)
            self._binaural_az.setEnabled(False)
            self._binaural_el.setEnabled(False)
            if not self._capabilities.queried:
                self._binaural_note.setText(
                    "C++ binaural capabilities were not reported (binary missing or older than this protocol). "
                    "The rest of the test bench remains usable."
                )
            else:
                self._binaural_note.setText("This C++ build reports binaural as unavailable.")
            return
        for name in caps.backends:
            self._binaural_backend.addItem(name, userData=name)
        unavailable = ", ".join(caps.unavailable_backends) or "none"
        self._binaural_note.setText(
            f"{caps.note} Unavailable backends (not offered): {unavailable}."
        )

    def _on_blend_changed(self, blend_deg: int) -> None:
        descriptor = "fully directional" if blend_deg == 0 else ("fully omnidirectional mix" if blend_deg >= 180 else "blended")
        self._blend_label.setText(f"Directional / Omni Blend: {blend_deg}° ({descriptor})")
        self._steering_dial.set_width_deg(blend_deg)

    def _on_select_file(self) -> None:
        paths, _ = QFileDialog.getOpenFileNames(self, "Select 6-channel audio file(s)", filter=_AUDIO_FILTER)
        if paths:
            self._set_selected_paths([Path(p) for p in paths])

    def _on_select_folder(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Select folder of audio files")
        if folder:
            self._set_selected_paths(audio_loader.find_audio_files(folder))

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
            validation = audio_loader.validate_audio_file(
                path,
                expected_sample_rate_hz=config_summary.capture_sample_rate_hz,
                active_channel_map=config_summary.active_channel_map,
            )
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
            kind = validation.error_kind or "validation"
            self._validation_label.setStyleSheet("color: #e05a5a;")
            self._validation_label.setText(f"Invalid ({kind}): " + "; ".join(validation.errors))

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

    def _current_binaural_request(self) -> BinauralRequest:
        backend = self._binaural_backend.currentData()
        return BinauralRequest(
            enabled=self._binaural_enable.isChecked() and self._binaural_enable.isEnabled(),
            backend=backend if isinstance(backend, str) else None,
            azimuth_deg=self._binaural_az.value(),
            elevation_deg=self._binaural_el.value(),
            follow_beamformer_steering=self._binaural_follow.isChecked(),
        )

    def _run_batch(self, paths: list[Path]) -> None:
        if self._batch_worker is not None and self._batch_worker.isRunning():
            QMessageBox.information(self, "Busy", "A batch is already processing.")
            return

        steering_events = [
            SteeringEvent(
                0.0,
                self._steering_dial.commanded_azimuth_deg(),
                0.0,
                width_deg=self._blend_slider.value(),
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
            suppression=self._suppression_combo.currentData() or SuppressionMode.AUTO.value,
            output_container=self._export_combo.currentData() or "wav",
            binaural=self._current_binaural_request(),
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
        stage_filename = self._stage_combo.currentData() or "processed_stereo.wav"
        residual_filename = self._residual_stage_combo.currentData() or "residual_beamform.wav"
        stage_path = test_root / stage_filename
        processed_path = stage_path if stage_path.exists() else test_root / "processed_stereo.wav"
        self.playback_panel.set_sources(
            raw=test_root / "raw_preview_stereo.wav",
            processed=processed_path,
            residual=test_root / residual_filename,
        )
        metrics = self._last_metrics.get(test_id) or self._result_store.load_metrics(test_id)
        self.metrics_panel.update_metrics(metrics)

        sweep = metrics.get("steering", {}).get("objective_sweep_test")
        self._steering_dial.set_estimated_azimuth_deg(sweep["measured_peak_azimuth_deg"] if sweep else None)

        try:
            processed_data, sample_rate = audio_loader.load_wav(processed_path)
            raw_data, _ = audio_loader.load_wav(test_root / "raw_preview_stereo.wav")
            residual_data, _ = audio_loader.load_wav(test_root / residual_filename)
            self.visualization_panel.plot_waveforms(sample_rate, raw=raw_data, processed=processed_data, residual=residual_data)
            self.visualization_panel.plot_spectrogram(processed_data, sample_rate)
            self.visualization_panel.plot_levels(processed_data, sample_rate)
        except Exception:  # noqa: BLE001 - visualization is best-effort
            pass
