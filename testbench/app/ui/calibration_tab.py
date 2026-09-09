"""Mode 3 — Calibration tab: multi-azimuth array imports, REW MDAT metadata,
compile YAML artifacts, and apply a variant to Recorded / Real-Time."""

from __future__ import annotations

from datetime import date
from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSpinBox,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from app.config_reader import DEFAULT_CONFIG_PATH
from app.controller.calibration_controller import (
    DEFAULT_CALIBRATION_OUT_DIR,
    MIC_IDS,
    STANDARD_ARRAY_AZIMUTHS_DEG,
    VARIANT_NOTES,
    VARIANT_ORDER,
    CalibrationCompileRequest,
    CalibrationWorker,
    default_geometry_path,
    format_azimuth_label,
    missing_compile_inputs,
    parse_rew_mdat,
    merge_mdat_results,
    summarize_mdat_markdown,
    write_dsp_runtime_overlay,
)
from app.controller.upload_controller import CalibrationCommitSnapshot
from app.ui.layout_persist import (
    KEY_CALIBRATION_H,
    CALIBRATION_H_DEFAULT,
    restore_splitter,
    save_splitter,
)
from app.ui.secondary_note import apply_secondary_note

_WAV_FILTER = "WAV files (*.wav);;All files (*)"
_YAML_FILTER = "YAML files (*.yaml *.yml);;All files (*)"
_TXT_FILTER = "Text files (*.txt);;All files (*)"
_MDAT_FILTER = "REW measurement (*.mdat);;All files (*)"

_COL_USE = 0
_COL_AZ = 1
_COL_WAV = 2
_COL_DELAY = 3
_COL_LEVEL = 4
_COL_STATUS = 5


def _scroll_area(inner: QWidget) -> QScrollArea:
    area = QScrollArea()
    area.setWidgetResizable(True)
    area.setFrameShape(QFrame.Shape.NoFrame)
    area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    area.setWidget(inner)
    return area


def _path_edit() -> QLineEdit:
    edit = QLineEdit()
    edit.setReadOnly(True)
    edit.setPlaceholderText("Not selected")
    return edit


class CalibrationTab(QWidget):
    dspConfigReady = Signal(str)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._config_path = DEFAULT_CONFIG_PATH
        self._worker: CalibrationWorker | None = None
        self._report: dict | None = None
        self._mdat_result = None
        self._layout_restored = False
        self._angle_wavs: dict[float, Path] = {}

        self._angle_table = QTableWidget(len(STANDARD_ARRAY_AZIMUTHS_DEG), 6)
        self._angle_table.setHorizontalHeaderLabels(
            ["Use", "Azimuth", "Array WAV", "MDAT delay ms", "Peak dBFS", "Status"]
        )
        self._angle_table.verticalHeader().setVisible(False)
        self._angle_table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self._angle_table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self._angle_table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        header = self._angle_table.horizontalHeader()
        header.setSectionResizeMode(_COL_USE, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(_COL_AZ, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(_COL_WAV, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(_COL_DELAY, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(_COL_LEVEL, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(_COL_STATUS, QHeaderView.ResizeMode.ResizeToContents)
        self._use_checks: dict[float, QCheckBox] = {}
        for row, az in enumerate(STANDARD_ARRAY_AZIMUTHS_DEG):
            use = QCheckBox()
            use.setChecked(abs(az) < 1e-9)
            use.setAccessibleName(f"Use array azimuth {format_azimuth_label(az)}")
            use.toggled.connect(lambda _checked, a=az: self._on_use_toggled(a))
            self._use_checks[az] = use
            self._angle_table.setCellWidget(row, _COL_USE, use)
            az_item = QTableWidgetItem(format_azimuth_label(az))
            az_item.setData(Qt.ItemDataRole.UserRole, float(az))
            self._angle_table.setItem(row, _COL_AZ, az_item)
            self._angle_table.setItem(row, _COL_WAV, QTableWidgetItem("—"))
            self._angle_table.setItem(row, _COL_DELAY, QTableWidgetItem("—"))
            self._angle_table.setItem(row, _COL_LEVEL, QTableWidgetItem("—"))
            self._angle_table.setItem(row, _COL_STATUS, QTableWidgetItem("bypassed" if abs(az) > 1e-9 else "needed"))
        self._angle_table.setMinimumHeight(280)

        angle_btns = QHBoxLayout()
        import_wav_btn = QPushButton("Import WAV for row")
        import_wav_btn.clicked.connect(self._on_import_angle_wav)
        import_mdat_btn = QPushButton("Import REW .mdat…")
        import_mdat_btn.setToolTip("Select one or more .mdat files (Ctrl+click). Split REW exports are merged.")
        import_mdat_btn.clicked.connect(self._on_import_mdat)
        enable_all_btn = QPushButton("Enable all")
        enable_all_btn.clicked.connect(lambda: self._set_all_angles(True))
        disable_extra_btn = QPushButton("Only 0°")
        disable_extra_btn.clicked.connect(self._enable_only_zero)
        angle_btns.addWidget(import_wav_btn)
        angle_btns.addWidget(import_mdat_btn)
        angle_btns.addWidget(enable_all_btn)
        angle_btns.addWidget(disable_extra_btn)
        angle_btns.addStretch(1)

        angle_note = QLabel(
            "Check angles to include. Unchecked rows are bypassed. "
            "Absolute TOF needs the played stimulus WAV; runtime YAML uses the primary enabled "
            "angle (prefers 0°). Extra enabled angles are estimated into the report only."
        )
        apply_secondary_note(angle_note)

        angle_box = QGroupBox("Array sweeps by azimuth (1 m)")
        angle_layout = QVBoxLayout(angle_box)
        angle_layout.addWidget(self._angle_table)
        angle_layout.addLayout(angle_btns)
        angle_layout.addWidget(angle_note)

        self._element_edits: list[QLineEdit] = []
        element_form = QFormLayout()
        element_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        for mic_id in MIC_IDS:
            edit = _path_edit()
            btn = QPushButton("Import")
            btn.setAccessibleName(f"Import element WAV for {mic_id}")
            btn.clicked.connect(lambda _checked=False, mid=mic_id: self._on_import_element(mid))
            row = QWidget()
            row_layout = QHBoxLayout(row)
            row_layout.setContentsMargins(0, 0, 0, 0)
            row_layout.addWidget(edit, stretch=1)
            row_layout.addWidget(btn)
            element_form.addRow(mic_id + ":", row)
            self._element_edits.append(edit)

        self._geometry_edit = _path_edit()
        self._geometry_edit.setText(str(default_geometry_path(self._config_path)))
        geometry_btn = QPushButton("Import geometry")
        geometry_btn.clicked.connect(self._on_import_geometry)

        self._rew_edit = _path_edit()
        rew_btn = QPushButton("Import REW filters")
        rew_btn.clicked.connect(self._on_import_rew)
        clear_rew_btn = QPushButton("Clear")
        clear_rew_btn.clicked.connect(lambda: self._rew_edit.setText(""))

        self._mdat_edit = _path_edit()
        self._mdat_edit.setPlaceholderText("Optional REW .mdat (select multiple if split at 32 measurements)")

        self._stimulus_edit = _path_edit()
        stimulus_btn = QPushButton("Import stimulus")
        stimulus_btn.clicked.connect(self._on_import_stimulus)
        clear_stimulus_btn = QPushButton("Clear")
        clear_stimulus_btn.clicked.connect(lambda: self._stimulus_edit.setText(""))

        self._out_edit = QLineEdit()
        self._out_edit.setText(str(DEFAULT_CALIBRATION_OUT_DIR / f"cal_{date.today().isoformat()}"))
        out_btn = QPushButton("Choose folder")
        out_btn.clicked.connect(self._on_choose_out_dir)
        self._tag_edit = QLineEdit("session")

        files_box = QGroupBox("Shared imports")
        files_layout = QVBoxLayout(files_box)
        files_layout.addWidget(QLabel("Same-position element sweeps (mono):"))
        files_layout.addLayout(element_form)
        geom_row = QHBoxLayout()
        geom_row.addWidget(self._geometry_edit, stretch=1)
        geom_row.addWidget(geometry_btn)
        files_layout.addWidget(QLabel("Geometry YAML:"))
        files_layout.addLayout(geom_row)
        stim_row = QHBoxLayout()
        stim_row.addWidget(self._stimulus_edit, stretch=1)
        stim_row.addWidget(stimulus_btn)
        stim_row.addWidget(clear_stimulus_btn)
        files_layout.addWidget(QLabel("Played stimulus WAV (required for absolute TOF):"))
        files_layout.addLayout(stim_row)
        mdat_row = QHBoxLayout()
        mdat_row.addWidget(self._mdat_edit, stretch=1)
        files_layout.addWidget(QLabel("Parsed REW .mdat (metadata / delays; multiple files are merged):"))
        files_layout.addLayout(mdat_row)
        rew_row = QHBoxLayout()
        rew_row.addWidget(self._rew_edit, stretch=1)
        rew_row.addWidget(rew_btn)
        rew_row.addWidget(clear_rew_btn)
        files_layout.addWidget(QLabel("Optional REW filter export:"))
        files_layout.addLayout(rew_row)
        out_row = QHBoxLayout()
        out_row.addWidget(self._out_edit, stretch=1)
        out_row.addWidget(out_btn)
        files_layout.addWidget(QLabel("Output folder:"))
        files_layout.addLayout(out_row)
        tag_row = QFormLayout()
        tag_row.addRow("Artifact tag:", self._tag_edit)
        files_layout.addLayout(tag_row)

        self._primary_combo = QComboBox()
        self._refresh_primary_combo()
        self._delay_mode = QComboBox()
        self._delay_mode.addItem("Absolute TOF (stimulus → mic)", userData="absolute_tof")
        self._delay_mode.addItem("Relative mic-vs-mic", userData="relative")
        self._elevation = QDoubleSpinBox()
        self._elevation.setRange(-90.0, 90.0)
        self._elevation.setSuffix("°")
        self._distance = QDoubleSpinBox()
        self._distance.setRange(0.1, 20.0)
        self._distance.setValue(1.0)
        self._distance.setSuffix(" m")
        self._wavefront = QComboBox()
        self._wavefront.addItem("Spherical (1 m)", userData="spherical")
        self._wavefront.addItem("Plane wave", userData="plane")
        self._max_lag = QSpinBox()
        self._max_lag.setRange(8, 48000)
        self._max_lag.setValue(4410)
        self._max_lag.setToolTip("Relative mode: ±max lag. Absolute TOF: use Max TOF below (0 = auto).")
        self._max_tof = QSpinBox()
        self._max_tof.setRange(0, 48000)
        self._max_tof.setValue(0)
        self._max_tof.setSpecialValueText("auto")
        self._max_tof.setToolTip("Absolute TOF search window in samples. 0 = distance/c + 100 ms.")
        self._polarity_threshold = QDoubleSpinBox()
        self._polarity_threshold.setRange(0.0, 1.0)
        self._polarity_threshold.setSingleStep(0.05)
        self._polarity_threshold.setValue(0.2)
        self._gain_source = QComboBox()
        self._gain_source.addItem("Element sweeps", userData="element")
        self._gain_source.addItem("Array sweep", userData="array")
        self._rew_max_q = QDoubleSpinBox()
        self._rew_max_q.setRange(0.1, 20.0)
        self._rew_max_q.setValue(4.0)
        self._rew_max_boost = QDoubleSpinBox()
        self._rew_max_boost.setRange(0.0, 24.0)
        self._rew_max_boost.setValue(6.0)
        self._rew_max_boost.setSuffix(" dB")
        self._rew_min_freq = QDoubleSpinBox()
        self._rew_min_freq.setRange(20.0, 8000.0)
        self._rew_min_freq.setValue(100.0)
        self._rew_min_freq.setSuffix(" Hz")
        self._m5_invert = QCheckBox("Emit M5 invert-test polarity on C/D/E variants")
        self._m5_invert.setChecked(True)

        controls_box = QGroupBox("Compiler controls")
        controls_form = QFormLayout(controls_box)
        controls_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        controls_form.addRow("Primary azimuth (runtime YAML):", self._primary_combo)
        controls_form.addRow("Delay mode:", self._delay_mode)
        controls_form.addRow("Source elevation:", self._elevation)
        controls_form.addRow("Source distance:", self._distance)
        controls_form.addRow("Wavefront:", self._wavefront)
        controls_form.addRow("Max relative lag (samples):", self._max_lag)
        controls_form.addRow("Max TOF (samples):", self._max_tof)
        controls_form.addRow("Polarity threshold:", self._polarity_threshold)
        controls_form.addRow("Gain source:", self._gain_source)
        controls_form.addRow("REW max Q:", self._rew_max_q)
        controls_form.addRow("REW max boost:", self._rew_max_boost)
        controls_form.addRow("REW min frequency:", self._rew_min_freq)
        controls_form.addRow(self._m5_invert)

        self._compile_btn = QPushButton("Compile calibration")
        self._compile_btn.setAccessibleName("Compile calibration artifacts")
        self._compile_btn.clicked.connect(self._on_compile)
        self._progress = QProgressBar()
        self._progress.setRange(0, 1)
        self._progress.setValue(0)
        self._status = QLabel("Idle")
        self._status.setWordWrap(True)

        config_inner = QWidget()
        config_layout = QVBoxLayout(config_inner)
        config_layout.setContentsMargins(0, 0, 0, 0)
        config_layout.addWidget(angle_box)
        config_layout.addWidget(files_box)
        config_layout.addWidget(controls_box)
        config_layout.addStretch(1)

        footer = QWidget()
        footer_layout = QVBoxLayout(footer)
        footer_layout.setContentsMargins(0, 0, 0, 0)
        footer_layout.addWidget(self._compile_btn)
        footer_layout.addWidget(self._progress)
        footer_layout.addWidget(self._status)

        left_layout = QVBoxLayout()
        left_layout.addWidget(_scroll_area(config_inner), stretch=1)
        left_layout.addWidget(footer)
        left_widget = QWidget()
        left_widget.setMinimumWidth(360)
        left_widget.setLayout(left_layout)

        self._variant_combo = QComboBox()
        for name in VARIANT_ORDER:
            self._variant_combo.addItem(name, userData=name)
            self._variant_combo.setItemData(
                self._variant_combo.count() - 1, VARIANT_NOTES[name], Qt.ItemDataRole.ToolTipRole
            )
        self._enable_common_eq = QCheckBox("Enable guarded REW common EQ in DSP overlay")
        self._apply_btn = QPushButton("Apply variant to Recorded / Real-Time")
        self._apply_btn.setEnabled(False)
        self._apply_btn.clicked.connect(self._on_apply)
        self._dsp_path_label = QLabel("DSP config: default.yaml")
        self._dsp_path_label.setWordWrap(True)
        self._dsp_path_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)

        apply_box = QGroupBox("Use in DSP tabs")
        apply_layout = QVBoxLayout(apply_box)
        apply_form = QFormLayout()
        apply_form.addRow("Variant:", self._variant_combo)
        apply_layout.addLayout(apply_form)
        apply_layout.addWidget(self._enable_common_eq)
        apply_layout.addWidget(self._apply_btn)
        apply_layout.addWidget(self._dsp_path_label)

        self._warnings = QLabel("")
        self._warnings.setWordWrap(True)
        self._report_view = QPlainTextEdit()
        self._report_view.setReadOnly(True)
        self._report_view.setPlaceholderText("Compile or import a .mdat to see metadata / reports here.")
        self._report_view.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        right_layout = QVBoxLayout()
        right_layout.addWidget(apply_box)
        right_layout.addWidget(self._warnings)
        right_layout.addWidget(self._report_view, stretch=1)
        right_widget = QWidget()
        right_widget.setMinimumWidth(360)
        right_widget.setLayout(right_layout)

        self._main_splitter = QSplitter(Qt.Orientation.Horizontal)
        self._main_splitter.setChildrenCollapsible(False)
        self._main_splitter.addWidget(left_widget)
        self._main_splitter.addWidget(right_widget)
        self._main_splitter.setStretchFactor(0, 48)
        self._main_splitter.setStretchFactor(1, 52)

        outer = QVBoxLayout(self)
        outer.addWidget(self._main_splitter)

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        if not self._layout_restored:
            restore_splitter(self._main_splitter, KEY_CALIBRATION_H, CALIBRATION_H_DEFAULT)
            self._layout_restored = True

    def save_layout(self) -> None:
        save_splitter(self._main_splitter, KEY_CALIBRATION_H)

    def shutdown(self) -> None:
        if self._worker is not None and self._worker.isRunning():
            self._worker.wait(30000)

    def calibration_commit_snapshot(self) -> CalibrationCommitSnapshot:
        if not self._report:
            return CalibrationCommitSnapshot(
                variant=str(self._variant_combo.currentData() or "E_full"),
                variant_yaml=None,
                common_eq_enabled=self._enable_common_eq.isChecked(),
                common_eq_sections=None,
            )
        variant = str(self._variant_combo.currentData() or "E_full")
        yaml_path_raw = self._report.get("emitted_yaml", {}).get(variant)
        common_eq = self._report.get("common_eq") or {}
        sections = common_eq.get("sections") if self._enable_common_eq.isChecked() else None
        return CalibrationCommitSnapshot(
            variant=variant,
            variant_yaml=Path(yaml_path_raw) if yaml_path_raw else None,
            common_eq_enabled=self._enable_common_eq.isChecked(),
            common_eq_sections=sections,
        )

    def _row_for_azimuth(self, azimuth_deg: float) -> int:
        for row in range(self._angle_table.rowCount()):
            item = self._angle_table.item(row, _COL_AZ)
            if item is not None and abs(float(item.data(Qt.ItemDataRole.UserRole)) - float(azimuth_deg)) < 1e-9:
                return row
        return -1

    def _set_row_status(self, azimuth_deg: float, status: str) -> None:
        row = self._row_for_azimuth(azimuth_deg)
        if row >= 0:
            self._angle_table.item(row, _COL_STATUS).setText(status)

    def _on_use_toggled(self, azimuth_deg: float) -> None:
        enabled = self._use_checks[azimuth_deg].isChecked()
        has_wav = azimuth_deg in self._angle_wavs
        if not enabled:
            self._set_row_status(azimuth_deg, "bypassed")
        elif has_wav:
            self._set_row_status(azimuth_deg, "ready")
        else:
            self._set_row_status(azimuth_deg, "needed")
        self._refresh_primary_combo()

    def _set_all_angles(self, enabled: bool) -> None:
        for az, check in self._use_checks.items():
            check.setChecked(enabled)
            self._on_use_toggled(az)

    def _enable_only_zero(self) -> None:
        for az, check in self._use_checks.items():
            check.setChecked(abs(az) < 1e-9)
            self._on_use_toggled(az)

    def _refresh_primary_combo(self) -> None:
        current = self._primary_combo.currentData()
        self._primary_combo.blockSignals(True)
        self._primary_combo.clear()
        enabled = [az for az, check in self._use_checks.items() if check.isChecked()]
        if not enabled:
            enabled = [0.0]
        for az in enabled:
            self._primary_combo.addItem(format_azimuth_label(az), userData=float(az))
        # Prefer 0° when available.
        prefer = 0.0 if any(abs(a) < 1e-9 for a in enabled) else enabled[0]
        if current is not None and any(abs(float(current) - a) < 1e-9 for a in enabled):
            prefer = float(current)
        idx = self._primary_combo.findData(prefer)
        self._primary_combo.setCurrentIndex(max(0, idx))
        self._primary_combo.blockSignals(False)

    def _on_import_angle_wav(self) -> None:
        row = self._angle_table.currentRow()
        if row < 0:
            QMessageBox.information(self, "Select a row", "Select an azimuth row first.")
            return
        az = float(self._angle_table.item(row, _COL_AZ).data(Qt.ItemDataRole.UserRole))
        path, _ = QFileDialog.getOpenFileName(
            self, f"Select 6-channel array WAV for {format_azimuth_label(az)}", filter=_WAV_FILTER
        )
        if not path:
            return
        self._angle_wavs[az] = Path(path)
        self._angle_table.item(row, _COL_WAV).setText(Path(path).name)
        self._angle_table.item(row, _COL_WAV).setToolTip(path)
        if not self._use_checks[az].isChecked():
            self._use_checks[az].setChecked(True)
        self._on_use_toggled(az)

    def _clear_mdat_preview_columns(self) -> None:
        for row, _az in enumerate(STANDARD_ARRAY_AZIMUTHS_DEG):
            self._angle_table.item(row, _COL_DELAY).setText("—")
            self._angle_table.item(row, _COL_DELAY).setToolTip("")
            self._angle_table.item(row, _COL_LEVEL).setText("—")

    def _apply_mdat_preview(self, result) -> None:
        self._clear_mdat_preview_columns()
        by_az = result.by_azimuth()
        for az in STANDARD_ARRAY_AZIMUTHS_DEG:
            row = self._row_for_azimuth(az)
            if row < 0:
                continue
            delays = result.array_channel_delays_ms(az)
            preferred = [
                m
                for m in result.measurements
                if m.is_array_channel
                and m.channel is not None
                and m.azimuth_deg is not None
                and abs(float(m.azimuth_deg) - float(az)) < 1e-6
            ]
            if delays and preferred:
                mean_ms = sum(delays.values()) / len(delays)
                self._angle_table.item(row, _COL_DELAY).setText(f"{mean_ms:.3f}")
                tip = ", ".join(f"ch{ch}:{ms:.3f}ms" for ch, ms in sorted(delays.items()))
                self._angle_table.item(row, _COL_DELAY).setToolTip(tip)
            peaks = [
                m.timing_peak_dbfs
                for m in by_az.get(float(az), [])
                if m.timing_peak_dbfs is not None and m.is_array_channel
            ]
            if peaks:
                self._angle_table.item(row, _COL_LEVEL).setText(f"{sum(peaks) / len(peaks):.1f}")

    def _on_import_mdat(self) -> None:
        paths, _ = QFileDialog.getOpenFileNames(self, "Select REW .mdat file(s)", filter=_MDAT_FILTER)
        if not paths:
            return
        parsed = []
        try:
            for path in paths:
                parsed.append(parse_rew_mdat(path))
            result = merge_mdat_results(parsed)
        except Exception as exc:  # noqa: BLE001
            QMessageBox.critical(self, "MDAT parse failed", str(exc))
            return
        self._mdat_result = result
        names = [p.name for p in (result.source_paths or [Path(p) for p in paths])]
        self._mdat_edit.setText(" + ".join(names))
        self._mdat_edit.setToolTip("\n".join(str(p) for p in (result.source_paths or paths)))
        self._report_view.setPlainText(summarize_mdat_markdown(result))
        self._apply_mdat_preview(result)
        self._status.setText(
            f"Parsed {len(result.source_paths or paths)} MDAT file(s): {len(result.measurements)} measurement notes"
        )
        self._warnings.setStyleSheet("color: #c9a227;")
        self._warnings.setText("; ".join(result.warnings))

    def _on_import_element(self, mic_id: str) -> None:
        idx = MIC_IDS.index(mic_id)
        path, _ = QFileDialog.getOpenFileName(self, f"Select mono element sweep for {mic_id}", filter=_WAV_FILTER)
        if path:
            self._element_edits[idx].setText(path)

    def _on_import_geometry(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Select geometry YAML", filter=_YAML_FILTER)
        if path:
            self._geometry_edit.setText(path)

    def _on_import_rew(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Select REW filter export", filter=_TXT_FILTER)
        if path:
            self._rew_edit.setText(path)

    def _on_import_stimulus(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Select played stimulus WAV", filter=_WAV_FILTER)
        if path:
            self._stimulus_edit.setText(path)

    def _on_choose_out_dir(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Select calibration output folder")
        if folder:
            self._out_edit.setText(folder)

    def _enabled_angle_wavs(self) -> list[tuple[float, Path]]:
        rows: list[tuple[float, Path]] = []
        for az, check in self._use_checks.items():
            if not check.isChecked():
                continue
            path = self._angle_wavs.get(az)
            if path is not None:
                rows.append((float(az), path))
        return rows

    def _current_request(self) -> CalibrationCompileRequest | None:
        enabled = self._enabled_angle_wavs()
        if not enabled:
            return None
        primary = float(self._primary_combo.currentData() if self._primary_combo.currentData() is not None else 0.0)
        primary_path = None
        extras: list[tuple[float, Path]] = []
        for az, path in enabled:
            if abs(az - primary) < 1e-9:
                primary_path = path
            else:
                extras.append((az, path))
        if primary_path is None:
            primary, primary_path = enabled[0]
            extras = [(az, path) for az, path in enabled[1:]]

        element_texts = [edit.text().strip() for edit in self._element_edits]
        element_paths = tuple(Path(text) if text else Path("") for text in element_texts)
        return CalibrationCompileRequest(
            array_wav=primary_path,
            element_wavs=element_paths,  # type: ignore[arg-type]
            geometry=Path(self._geometry_edit.text().strip() or ""),
            out_dir=Path(self._out_edit.text().strip() or DEFAULT_CALIBRATION_OUT_DIR),
            tag=self._tag_edit.text().strip() or "session",
            azimuth_deg=float(primary),
            elevation_deg=float(self._elevation.value()),
            distance_m=float(self._distance.value()),
            wavefront=str(self._wavefront.currentData() or "spherical"),
            max_lag_samples=int(self._max_lag.value()),
            polarity_threshold=float(self._polarity_threshold.value()),
            gain_source=str(self._gain_source.currentData() or "element"),
            rew_filter_txt=self._rew_edit.text().strip(),
            rew_max_q=float(self._rew_max_q.value()),
            rew_max_boost_db=float(self._rew_max_boost.value()),
            rew_min_freq_hz=float(self._rew_min_freq.value()),
            ch6_invert_test=self._m5_invert.isChecked(),
            extra_array_angles=tuple(extras),
            stimulus_wav=self._stimulus_edit.text().strip(),
            delay_mode=str(self._delay_mode.currentData() or "absolute_tof"),
            max_tof_samples=int(self._max_tof.value()),
        )

    def _on_compile(self) -> None:
        if self._worker is not None and self._worker.isRunning():
            QMessageBox.information(self, "Busy", "A calibration compile is already running.")
            return
        request = self._current_request()
        if request is None:
            QMessageBox.warning(
                self,
                "No enabled array WAV",
                "Enable at least one azimuth and import its 6-channel array WAV.",
            )
            return
        missing = missing_compile_inputs(request)
        # Extra angle WAVs already validated by presence in _enabled_angle_wavs.
        for az, path in request.extra_array_angles:
            if not path.is_file():
                missing.append(f"array WAV {format_azimuth_label(az)}")
        if missing:
            QMessageBox.warning(self, "Missing inputs", "Select: " + ", ".join(missing) + ".")
            return
        self._compile_btn.setEnabled(False)
        self._apply_btn.setEnabled(False)
        self._progress.setRange(0, 0)
        self._status.setText(
            f"Compiling primary {format_azimuth_label(request.azimuth_deg)}"
            + (f" + {len(request.extra_array_angles)} extra angle(s)…" if request.extra_array_angles else "…")
        )
        worker = CalibrationWorker(request)
        worker.finished_ok.connect(self._on_compile_ok)
        worker.failed.connect(self._on_compile_failed)
        self._worker = worker
        worker.start()

    def _on_compile_ok(self, report: object) -> None:
        self._progress.setRange(0, 1)
        self._progress.setValue(1)
        self._compile_btn.setEnabled(True)
        self._report = report if isinstance(report, dict) else None
        if self._report is None:
            self._status.setText("Compile finished with an unexpected result.")
            return
        self._apply_btn.setEnabled(True)
        warnings = self._report.get("warnings") or []
        if warnings:
            self._warnings.setStyleSheet("color: #c9a227;")
            self._warnings.setText("Warnings: " + "; ".join(str(w) for w in warnings))
        else:
            self._warnings.setStyleSheet("color: #4caf50;")
            self._warnings.setText("No compiler warnings.")
        report_md = Path(self._out_edit.text().strip()) / "calibration_report.md"
        chunks: list[str] = []
        if self._mdat_result is not None:
            chunks.append(summarize_mdat_markdown(self._mdat_result).rstrip())
            chunks.append("")
        if report_md.is_file():
            chunks.append(report_md.read_text(encoding="utf-8"))
        angles = self._report.get("angles") or []
        if angles:
            chunks.append("\n## Array angles estimated\n")
            for angle in angles:
                chunks.append(
                    f"- {format_azimuth_label(angle['azimuth_deg'])} ({angle['role']}): `{Path(angle['wav_path']).name}`"
                )
        self._report_view.setPlainText("\n".join(chunks))
        self._status.setText(f"Wrote artifacts to {self._out_edit.text().strip()}")

    def _on_compile_failed(self, error: str) -> None:
        self._progress.setRange(0, 1)
        self._progress.setValue(0)
        self._compile_btn.setEnabled(True)
        self._status.setText(f"Failed: {error}")
        QMessageBox.critical(self, "Calibration compile failed", error)

    def _on_apply(self) -> None:
        if not self._report:
            QMessageBox.warning(self, "No artifacts", "Compile a calibration first.")
            return
        variant = str(self._variant_combo.currentData() or "E_full")
        yaml_path = self._report.get("emitted_yaml", {}).get(variant)
        if not yaml_path:
            QMessageBox.warning(self, "Missing variant", f"No YAML for {variant}.")
            return
        out_dir = Path(self._out_edit.text().strip())
        overlay = out_dir / "runtime_config_overlay.yaml"
        common_eq = self._report.get("common_eq") or {}
        sections = common_eq.get("sections") if self._enable_common_eq.isChecked() else None
        try:
            written = write_dsp_runtime_overlay(
                overlay,
                calibration_yaml=yaml_path,
                base_config=self._config_path,
                common_eq_enabled=self._enable_common_eq.isChecked(),
                common_eq_sections=sections,
            )
        except Exception as exc:  # noqa: BLE001
            QMessageBox.critical(self, "Could not write DSP overlay", str(exc))
            return
        self._dsp_path_label.setText(f"DSP config: {written}")
        self._status.setText(f"Applied {variant} to Recorded and Real-Time tabs.")
        self.dspConfigReady.emit(str(written))
