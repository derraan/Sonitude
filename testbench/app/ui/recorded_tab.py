"""Mode 1 — Recorded Data tab: select audio file(s)/folder, inspect metadata,
run the existing algorithm via BatchWorker, and inspect results."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt, QThread, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from app.audio_io import audio_loader
from app.audio_io.stream_io import FileOverview, load_file_overviews_parallel
from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.controller.batch_controller import BatchWorker
from app.controller.file_preview_controller import FilePreviewWorker
from app.processing.capabilities import query_tool_capabilities
from app.storage.models import SteeringEvent, SuppressorRequest
from app.storage.result_store import ResultStore
from app.ui.beamformer_controls import BeamformerControls
from app.ui.binaural_controls import BinauralControls
from app.ui.layout_persist import (
    KEY_RECORDED_H,
    KEY_RECORDED_V,
    RECORDED_H_DEFAULT,
    RECORDED_V_DEFAULT,
    restore_splitter,
    save_splitter,
)
from app.ui.metrics_panel import MetricsPanel
from app.ui.playback_panel import PlaybackPanel
from app.ui.secondary_note import apply_secondary_note
from app.ui.steering_controls import SteeringControls
from app.ui.suppressor_controls import SuppressorControls
from app.ui.visualization_panel import VisualizationPanel

_METADATA_FIELDS = [
    ("filename", "Filename"),
    ("container", "Format"),
    ("sample_rate_hz", "Sample rate"),
    ("channels", "Channels"),
    ("bit_depth", "Bit depth"),
    ("duration_s", "Duration"),
    ("num_samples", "Samples"),
]
_AUDIO_FILTER = "Audio files (*.wav *.flac *.mp3);;WAV (*.wav);;FLAC (*.flac);;MP3 (*.mp3)"
_PLOT_STAGE_ITEMS = (
    (
        "Ear-cup preview",
        "raw_preview_stereo.wav",
        "Listening-only ear-cup stereo (map 4/5). Not binaural. Never used in residuals.",
    ),
    (
        "Beamformed",
        "beamformed.wav",
        "C++ tap, pre-suppression mono (stereo-duplicated for listen).",
    ),
    ("Suppressed", "suppressed.wav", "C++ tap, pre-limiter mono."),
    (
        "Binaural",
        "binaural_stereo.wav",
        "C++ binaural tap if present. Not the ear-cup preview.",
    ),
    ("Final output", "processed_stereo.wav", "Post-limiter processed stereo."),
)
_RESIDUAL_ITEMS = (
    ("Beamform residual", "residual_beamform.wav", "beamformed − suppressed"),
    ("Limiter residual", "residual_limiter.wav", "suppressed − processed"),
)


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


class _PlotOverviewWorker(QThread):
    loaded = Signal(int, object)

    def __init__(
        self,
        generation: int,
        specs: dict[str, tuple[Path, dict[str, Any]]],
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._generation = generation
        self._specs = specs

    def run(self) -> None:
        self.loaded.emit(self._generation, load_file_overviews_parallel(self._specs))


class RecordedDataTab(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._config_path = DEFAULT_CONFIG_PATH
        self._selected_paths: list[Path] = []
        self._result_store = ResultStore()
        self._batch_worker: BatchWorker | None = None
        self._batch_failures: list[str] = []
        self._batch_file_count = 0
        self._last_metrics: dict[str, dict] = {}
        self._capabilities = query_tool_capabilities("sonitude_stream_process")
        self._plot_cache: dict = {}
        self._plot_load_generation = 0
        self._plot_overview_worker: _PlotOverviewWorker | None = None
        self._layout_restored = False
        self._preview: FilePreviewWorker | None = None

        select_row = QHBoxLayout()
        select_file_btn = QPushButton("Select Audio")
        select_folder_btn = QPushButton("Select Folder")
        select_file_btn.clicked.connect(self._on_select_file)
        select_folder_btn.clicked.connect(self._on_select_folder)
        select_row.addWidget(select_file_btn)
        select_row.addWidget(select_folder_btn)

        self._file_list = QListWidget()
        self._file_list.setMaximumHeight(140)
        self._file_list.currentRowChanged.connect(self._on_file_row_changed)

        self._metadata_labels: dict[str, QLabel] = {}
        metadata_form = QFormLayout()
        metadata_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        metadata_form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        for key, title in _METADATA_FIELDS:
            value = QLabel("—")
            value.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            value.setWordWrap(True)
            self._metadata_labels[key] = value
            metadata_form.addRow(title + ":", value)
        metadata_box = QGroupBox("Input information")
        metadata_box.setLayout(metadata_form)

        self._validation_label = QLabel("")
        self._validation_label.setWordWrap(True)

        input_widget = QWidget()
        input_layout = QVBoxLayout(input_widget)
        input_layout.setContentsMargins(0, 0, 0, 0)
        input_layout.addLayout(select_row)
        input_layout.addWidget(self._file_list)
        input_layout.addWidget(metadata_box)
        input_layout.addWidget(self._validation_label)

        self._steering = SteeringControls("Beamformer steering (MVDR)")

        self._beamformer = BeamformerControls()

        self._suppressor = SuppressorControls(self._capabilities)

        export_box = QGroupBox("Final result export")
        self._export_combo = QComboBox()
        _compact_combo(self._export_combo)
        self._export_combo.addItem("WAV (float32)", userData="wav")
        self._export_combo.addItem("FLAC (PCM_24 lossless integer)", userData="flac")
        export_note = QLabel("Export container does not change DSP. Intermediates stay WAV.")
        apply_secondary_note(export_note)
        export_layout = QVBoxLayout(export_box)
        export_layout.addWidget(self._export_combo)
        export_layout.addWidget(export_note)

        self._binaural = BinauralControls(self._capabilities)
        self._live_dsp = QCheckBox("Live DSP — steering and binaural apply while playing")
        self._live_dsp.setChecked(True)
        self._live_dsp.setToolTip(
            "Play the selected 6-channel file through sonitude_stream_process. "
            "Turn the dial or binaural controls and hear the change immediately, like a plugin."
        )
        self._live_dsp.toggled.connect(self._on_live_dsp_toggled)

        steering_test_box = QGroupBox("Objective Steering Test (optional, slower)")
        self._steering_test_checkbox = QCheckBox("Run steering sweep for this batch")
        self._expected_azimuth_spin = QDoubleSpinBox()
        self._expected_azimuth_spin.setRange(-180.0, 180.0)
        self._expected_azimuth_spin.setSuffix("°")
        self._expected_azimuth_spin.setToolTip(
            "Known/expected direction of the dominant source in the recording, used as ground "
            "truth to score the measured beam-response-peak sweep."
        )
        steering_test_layout = QFormLayout(steering_test_box)
        steering_test_layout.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        steering_test_layout.addRow(self._steering_test_checkbox)
        steering_test_layout.addRow("Expected source direction:", self._expected_azimuth_spin)

        config_inner = QWidget()
        config_layout = QVBoxLayout(config_inner)
        config_layout.addWidget(self._steering)
        config_layout.addWidget(self._beamformer)
        config_layout.addWidget(self._live_dsp)
        config_layout.addWidget(self._suppressor)
        config_layout.addWidget(export_box)
        config_layout.addWidget(self._binaural)
        config_layout.addWidget(steering_test_box)
        config_layout.addStretch(1)

        self._process_selected_btn = QPushButton("Process Selected")
        self._process_batch_btn = QPushButton("Process Batch")
        self._process_selected_btn.clicked.connect(self._on_process_selected)
        self._process_batch_btn.clicked.connect(self._on_process_batch)
        process_row = QHBoxLayout()
        process_row.addWidget(self._process_selected_btn)
        process_row.addWidget(self._process_batch_btn)
        self._progress_bar = QProgressBar()
        self._status_label = QLabel("Idle")
        self._status_label.setWordWrap(True)

        footer = QWidget()
        footer_layout = QVBoxLayout(footer)
        footer_layout.setContentsMargins(0, 0, 0, 0)
        footer_layout.addLayout(process_row)
        footer_layout.addWidget(self._progress_bar)
        footer_layout.addWidget(self._status_label)

        left_layout = QVBoxLayout()
        left_layout.addWidget(input_widget)
        left_layout.addWidget(_scroll_area(config_inner), stretch=1)
        left_layout.addWidget(footer)
        left_widget = QWidget()
        left_widget.setMinimumWidth(280)
        left_widget.setLayout(left_layout)

        self._results_combo = QComboBox()
        _compact_combo(self._results_combo)
        self._results_combo.currentTextChanged.connect(self._on_result_selected)
        self._stage_combo = QComboBox()
        _compact_combo(self._stage_combo)
        for label, filename, tip in _PLOT_STAGE_ITEMS:
            self._stage_combo.addItem(label, userData=filename)
            self._stage_combo.setItemData(self._stage_combo.count() - 1, tip, Qt.ItemDataRole.ToolTipRole)
        self._stage_combo.currentIndexChanged.connect(lambda _i: self._on_result_selected(self._results_combo.currentText()))
        self._residual_stage_combo = QComboBox()
        _compact_combo(self._residual_stage_combo)
        for label, filename, tip in _RESIDUAL_ITEMS:
            self._residual_stage_combo.addItem(label, userData=filename)
            self._residual_stage_combo.setItemData(self._residual_stage_combo.count() - 1, tip, Qt.ItemDataRole.ToolTipRole)
        self._residual_stage_combo.currentIndexChanged.connect(
            lambda _i: self._on_result_selected(self._results_combo.currentText())
        )

        inspect_form = QFormLayout()
        inspect_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        inspect_form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        inspect_form.addRow("Result:", self._results_combo)
        inspect_form.addRow("Plot stage:", self._stage_combo)
        inspect_form.addRow("Plot residual:", self._residual_stage_combo)

        domain_note = QLabel(
            "Plot stage and Plot residual choose which DSP files are plotted and which files the "
            "Listen-to radios play. Ear-cup preview is listening-only and is never a residual."
        )
        apply_secondary_note(domain_note)

        self.playback_panel = PlaybackPanel()
        self.playback_panel.sourceSelected.connect(self._on_listen_source_changed)
        self.playback_panel.set_live_mode(True)
        self.playback_panel.livePlayRequested.connect(self._on_live_play)
        self.playback_panel.livePauseRequested.connect(self._on_live_pause)
        self.playback_panel.liveStopRequested.connect(self._on_live_stop)
        self.playback_panel.liveSeekRequested.connect(self._on_live_seek)
        self.playback_panel.volumeChanged.connect(self._on_live_volume)
        self.playback_panel.boostChanged.connect(self._on_live_boost)
        self._steering.azimuthChanged.connect(self._push_live_params)
        self._steering.blendChanged.connect(lambda _w: self._push_live_params())
        self._beamformer.changed.connect(self._push_live_params)
        self._suppressor.changed.connect(self._push_live_params)
        self._suppressor.backendChanged.connect(self._on_suppression_backend_changed)
        self._binaural.changed.connect(self._push_live_params)
        self.visualization_panel = VisualizationPanel()
        self.visualization_panel.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self.metrics_panel = MetricsPanel()
        metrics_scroll = _scroll_area(self.metrics_panel)
        metrics_scroll.setMinimumHeight(80)

        self._inspect_splitter = QSplitter(Qt.Orientation.Vertical)
        self._inspect_splitter.setChildrenCollapsible(False)
        self._inspect_splitter.addWidget(self.visualization_panel)
        self._inspect_splitter.addWidget(metrics_scroll)
        self._inspect_splitter.setStretchFactor(0, 3)
        self._inspect_splitter.setStretchFactor(1, 1)

        right_layout = QVBoxLayout()
        right_layout.addLayout(inspect_form)
        right_layout.addWidget(domain_note)
        right_layout.addWidget(self.playback_panel)
        right_layout.addWidget(self._inspect_splitter, stretch=1)
        right_widget = QWidget()
        right_widget.setMinimumWidth(400)
        right_widget.setLayout(right_layout)

        self._main_splitter = QSplitter(Qt.Orientation.Horizontal)
        self._main_splitter.setChildrenCollapsible(False)
        self._main_splitter.addWidget(left_widget)
        self._main_splitter.addWidget(right_widget)
        self._main_splitter.setStretchFactor(0, 38)
        self._main_splitter.setStretchFactor(1, 62)

        outer = QVBoxLayout(self)
        outer.addWidget(self._main_splitter)

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        if not self._layout_restored:
            restore_splitter(self._main_splitter, KEY_RECORDED_H, RECORDED_H_DEFAULT)
            restore_splitter(self._inspect_splitter, KEY_RECORDED_V, RECORDED_V_DEFAULT)
            self._layout_restored = True

    def save_layout(self) -> None:
        save_splitter(self._main_splitter, KEY_RECORDED_H)
        save_splitter(self._inspect_splitter, KEY_RECORDED_V)

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
            for key, _title in _METADATA_FIELDS:
                text = str(data.get(key, ""))
                label = self._metadata_labels[key]
                label.setText(text)
                label.setToolTip(text)

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

    def _run_batch(self, paths: list[Path]) -> None:
        if self._batch_worker is not None and self._batch_worker.isRunning():
            QMessageBox.information(self, "Busy", "A batch is already processing.")
            return

        steering_events = [
            SteeringEvent(
                0.0,
                self._steering.commanded_azimuth_deg(),
                0.0,
                width_deg=self._steering.width_deg(),
            )
        ]
        self._progress_bar.setRange(0, 100 if len(paths) == 1 else len(paths))
        self._progress_bar.setValue(0)
        self._status_label.setText(f"Processing 0/{len(paths)}...")

        expected_azimuth = (
            self._expected_azimuth_spin.value() if self._steering_test_checkbox.isChecked() else None
        )
        worker = BatchWorker(
            paths,
            self._config_path,
            steering_events,
            suppression=self._suppressor.suppression_mode(),
            suppression_backend=self._suppressor.suppression_backend(),
            output_container=self._export_combo.currentData() or "wav",
            binaural=self._binaural.request(),
            steering_test_expected_azimuth_deg=expected_azimuth,
            result_store=self._result_store,
        )
        worker.progress.connect(self._on_batch_progress)
        worker.file_progress.connect(self._on_file_progress)
        worker.file_finished.connect(self._on_file_finished)
        worker.file_failed.connect(self._on_file_failed)
        worker.finished_all.connect(self._on_batch_finished)
        self._batch_failures = []
        self._batch_worker = worker
        self._batch_file_count = len(paths)
        worker.start()

    def _on_file_progress(self, percent: int) -> None:
        if self._batch_file_count == 1:
            self._progress_bar.setValue(percent)
            self._status_label.setText(f"Processing {percent}%...")

    def _on_batch_progress(self, done: int, total: int) -> None:
        if total > 1:
            self._progress_bar.setRange(0, total)
            self._progress_bar.setValue(done)
            self._status_label.setText(f"Processing {done}/{total}...")

    def _on_file_finished(self, input_path: str, result: dict) -> None:
        test_id = result["test_id"]
        self._last_metrics[test_id] = result["metrics"]
        self._results_combo.addItem(test_id)
        binaural_path = self._result_store.results_dir / test_id / "binaural_stereo.wav"
        if binaural_path.exists():
            binaural_index = self._stage_combo.findData("binaural_stereo.wav")
            if binaural_index >= 0:
                self._stage_combo.setCurrentIndex(binaural_index)
        self._results_combo.setCurrentText(test_id)
        backend = result["metrics"].get("suppression_backend_resolved") or "yaml"
        self._status_label.setText(
            f"Finished: {Path(input_path).name} -> {test_id} (backend {backend})"
        )

    def _on_file_failed(self, input_path: str, error: str) -> None:
        name = Path(input_path).name
        self._batch_failures.append(name)
        self._status_label.setText(f"Failed: {name}: {error}")
        QMessageBox.critical(self, "Processing failed", f"{name}\n\n{error}")

    def _on_batch_finished(self) -> None:
        if self._batch_failures:
            self._status_label.setText(f"Finished with {len(self._batch_failures)} failure(s).")
            return
        self._status_label.setText("Done.")

    def _on_live_dsp_toggled(self, checked: bool) -> None:
        if not checked:
            self._on_live_stop()
        self.playback_panel.set_live_mode(checked)

    def _selected_input_path(self) -> Path | None:
        row = self._file_list.currentRow()
        if row < 0 or row >= len(self._selected_paths):
            return None
        return self._selected_paths[row]

    def _on_suppression_backend_changed(self) -> None:
        if self._preview is not None and self._preview.isRunning():
            self._on_live_stop()
            self._on_live_play()

    def _live_dsp_request(self) -> SuppressorRequest:
        request = self._suppressor.request()
        self._beamformer.apply_to_request(request)
        return request

    def _push_live_params(self, *_args) -> None:
        if self._preview is None or not self._preview.isRunning():
            return
        self._preview.set_steering(self._steering.commanded_azimuth_deg(), 0.0, self._steering.width_deg())
        self._preview.set_binaural(self._binaural.request())
        self._preview.set_suppressor(self._live_dsp_request())

    def _on_live_volume(self, volume: float) -> None:
        if self._preview is not None:
            self._preview.set_volume(volume)

    def _on_live_boost(self, preamp_db: float) -> None:
        if self._preview is not None:
            self._preview.set_preamp_db(preamp_db)

    def _on_live_play(self) -> None:
        if self._preview is not None and self._preview.isRunning():
            self._preview.resume()
            return
        path = self._selected_input_path()
        if path is None:
            QMessageBox.warning(self, "No file selected", "Select a 6-channel recording first.")
            return
        try:
            config = read_runtime_config_summary(self._config_path)
        except Exception as exc:  # noqa: BLE001
            QMessageBox.critical(self, "Malformed configuration", str(exc))
            return
        self._on_live_stop()
        worker = FilePreviewWorker(
            path,
            self._config_path,
            config.capture_sample_rate_hz,
            active_channel_map=config.active_channel_map,
            suppression=self._suppressor.suppression_mode(),
            suppression_backend=self._suppressor.suppression_backend(),
            binaural=self._binaural.request(),
            suppressor=self._live_dsp_request(),
        )
        worker.set_steering(self._steering.commanded_azimuth_deg(), 0.0, self._steering.width_deg())
        worker.set_volume(self.playback_panel.volume())
        worker.set_preamp_db(self.playback_panel.preamp_db())
        worker.positionChanged.connect(
            lambda ms: self.playback_panel.set_clock(ms, worker.duration_ms())
        )
        worker.durationChanged.connect(lambda ms: self.playback_panel.set_clock(0, ms))
        worker.blockProcessed.connect(self._on_live_block)
        worker.errorOccurred.connect(lambda msg: QMessageBox.critical(self, "Live DSP error", msg))
        worker.stopped.connect(self._on_live_worker_stopped)
        self._preview = worker
        worker.start()
        self._status_label.setText(f"Live DSP: {path.name}")

    def _on_live_pause(self) -> None:
        if self._preview is not None:
            self._preview.pause()

    def _on_live_stop(self) -> None:
        if self._preview is not None and self._preview.isRunning():
            self._preview.request_stop()
            self._preview.wait(2000)
        self._preview = None

    def _on_live_seek(self, position_ms: int) -> None:
        if self._preview is not None:
            self._preview.seek_ms(position_ms)

    def _on_live_block(self, raw_block, processed_block) -> None:
        rate = self._plot_cache.get("sample_rate") or 44100
        self.visualization_panel.plot_waveforms(rate, raw=raw_block, processed=processed_block)

    def _on_live_worker_stopped(self) -> None:
        if "Error" not in self._status_label.text() and "Live DSP" in self._status_label.text():
            self._status_label.setText("Live DSP stopped")

    def _on_listen_source_changed(self, name: str) -> None:
        cache = self._plot_cache
        if not cache:
            return
        self.visualization_panel.plot_waveforms(
            cache["sample_rate"],
            raw=cache.get("raw"),
            processed=cache.get("processed"),
            residual=cache.get("residual"),
            emphasize=name,
            duration_s=cache.get("duration_s"),
        )

    def _on_result_selected(self, test_id: str) -> None:
        if not test_id:
            return
        test_root = self._result_store.results_dir / test_id
        stage_filename = self._stage_combo.currentData() or "processed_stereo.wav"
        residual_filename = self._residual_stage_combo.currentData() or "residual_beamform.wav"
        stage_path = test_root / stage_filename
        processed_path = stage_path if stage_path.exists() else test_root / "processed_stereo.wav"
        residual_path = test_root / residual_filename
        raw_path = test_root / "raw_preview_stereo.wav"
        self.playback_panel.set_sources(
            raw=raw_path if raw_path.exists() else None,
            processed=processed_path if processed_path.exists() else None,
            residual=residual_path if residual_path.exists() else None,
        )
        metrics = self._last_metrics.get(test_id) or self._result_store.load_metrics(test_id)
        self.metrics_panel.update_metrics(metrics)

        sweep = metrics.get("steering", {}).get("objective_sweep_test")
        self._steering.set_estimated_azimuth_deg(sweep["measured_peak_azimuth_deg"] if sweep else None)

        specs: dict[str, tuple[Path, dict[str, Any]]] = {"processed": (processed_path, {})}
        if raw_path.exists():
            specs["raw"] = (raw_path, {"n_spec": 0})
        if residual_path.exists():
            specs["residual"] = (residual_path, {"n_spec": 0})

        self._plot_load_generation += 1
        generation = self._plot_load_generation
        worker = _PlotOverviewWorker(generation, specs, parent=self)
        worker.loaded.connect(self._on_plot_overviews_loaded)
        self._plot_overview_worker = worker
        worker.start()

    def _on_plot_overviews_loaded(self, generation: int, overviews: dict[str, FileOverview]) -> None:
        if generation != self._plot_load_generation:
            return
        overview = overviews.get("processed")
        if overview is None:
            self._plot_cache = {}
            return
        raw_overview = overviews.get("raw")
        residual_overview = overviews.get("residual")
        duration_s = overview.duration_s
        sample_rate = overview.sample_rate_hz
        self._plot_cache = {
            "sample_rate": sample_rate,
            "raw": None if raw_overview is None else raw_overview.waveform,
            "processed": overview.waveform,
            "residual": None if residual_overview is None else residual_overview.waveform,
            "duration_s": duration_s,
        }
        self.visualization_panel.plot_waveforms(
            sample_rate,
            raw=self._plot_cache["raw"],
            processed=overview.waveform,
            residual=self._plot_cache["residual"],
            emphasize=self.playback_panel.listen_source(),
            duration_s=duration_s,
        )
        self.visualization_panel.plot_spectrogram(
            overview.waveform,
            sample_rate,
            duration_s=duration_s,
            freqs=overview.spectrogram_freqs,
            spectrogram_db=overview.spectrogram_db,
        )
        self.visualization_panel.plot_levels(
            overview.waveform,
            sample_rate,
            duration_s=duration_s,
            rms_dbfs=overview.rms_dbfs,
        )
