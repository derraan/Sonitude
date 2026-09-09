"""Mode 4 — Upload tab: commit selected calibration + DSP knobs into default YAML."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QFormLayout,
    QFrame,
    QGroupBox,
    QLabel,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from app.config_reader import DEFAULT_CONFIG_PATH
from app.controller.upload_controller import (
    CalibrationCommitSnapshot,
    DspCommitSnapshot,
    UploadCommitResult,
    commit_runtime_config,
)
from app.ui.layout_persist import KEY_UPLOAD_H, UPLOAD_H_DEFAULT, restore_splitter, save_splitter
from app.ui.secondary_note import apply_secondary_note


def _scroll_area(inner: QWidget) -> QScrollArea:
    area = QScrollArea()
    area.setWidgetResizable(True)
    area.setFrameShape(QFrame.Shape.NoFrame)
    area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    area.setWidget(inner)
    return area


class UploadTab(QWidget):
    runtimeConfigCommitted = Signal(str)

    def __init__(self, calibration_tab, recorded_tab, realtime_tab, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._calibration_tab = calibration_tab
        self._recorded_tab = recorded_tab
        self._realtime_tab = realtime_tab
        self._layout_restored = False

        self._source_combo = QComboBox()
        self._source_combo.addItem("Real-Time Audio", userData="realtime")
        self._source_combo.addItem("Recorded Data", userData="recorded")
        self._source_combo.currentIndexChanged.connect(self._refresh_summary)

        self._refresh_btn = QPushButton("Refresh Summary")
        self._refresh_btn.clicked.connect(self._refresh_summary)

        source_row = QFormLayout()
        source_row.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        source_row.addRow("Commit DSP settings from:", self._source_combo)

        self._mismatch_label = QLabel("")
        self._mismatch_label.setWordWrap(True)

        self._calibration_label = QLabel("Calibration variant: —")
        self._calibration_label.setWordWrap(True)
        self._calibration_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)

        self._summary = QPlainTextEdit()
        self._summary.setReadOnly(True)
        self._summary.setPlaceholderText("Select calibration and tune Recorded/Real-Time settings first.")

        self._commit_btn = QPushButton("Commit to RT YAML")
        self._commit_btn.clicked.connect(self._on_commit)

        self._status = QLabel("Ready.")
        self._status.setWordWrap(True)
        self._status.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self._paths = QLabel("")
        self._paths.setWordWrap(True)
        self._paths.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)

        restart_note = QLabel(
            "Upload writes files only. Running Recorded/Real-Time sessions are not hot-reloaded; "
            "restart them to apply committed calibration/YAML settings."
        )
        apply_secondary_note(restart_note)
        persisted_note = QLabel(
            "Not persisted here: directional/omni blend, commanded azimuth, live preamp, "
            "live confidence/focus, and suppressor envelope attack/release."
        )
        apply_secondary_note(persisted_note)

        top_box = QGroupBox("Upload compiled calibration to runtime config")
        top_layout = QVBoxLayout(top_box)
        top_layout.addLayout(source_row)
        top_layout.addWidget(self._refresh_btn)
        top_layout.addWidget(self._mismatch_label)
        top_layout.addWidget(self._calibration_label)
        top_layout.addWidget(self._summary, stretch=1)
        top_layout.addWidget(self._commit_btn)
        top_layout.addWidget(self._status)
        top_layout.addWidget(self._paths)
        top_layout.addWidget(restart_note)
        top_layout.addWidget(persisted_note)

        self._details = QPlainTextEdit()
        self._details.setReadOnly(True)
        self._details.setPlaceholderText("Last commit details will appear here.")

        self._main_splitter = QSplitter(Qt.Orientation.Vertical)
        self._main_splitter.setChildrenCollapsible(False)
        self._main_splitter.addWidget(_scroll_area(top_box))
        self._main_splitter.addWidget(self._details)
        self._main_splitter.setStretchFactor(0, 70)
        self._main_splitter.setStretchFactor(1, 30)

        layout = QVBoxLayout(self)
        layout.addWidget(self._main_splitter)
        self._refresh_summary()

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        if not self._layout_restored:
            restore_splitter(self._main_splitter, KEY_UPLOAD_H, UPLOAD_H_DEFAULT)
            self._layout_restored = True
        self._refresh_summary()

    def save_layout(self) -> None:
        save_splitter(self._main_splitter, KEY_UPLOAD_H)

    def _selected_dsp_snapshot(self) -> DspCommitSnapshot:
        source = self._source_combo.currentData()
        if source == "recorded":
            return self._recorded_tab.dsp_commit_snapshot()
        return self._realtime_tab.dsp_commit_snapshot()

    def _refresh_summary(self) -> None:
        cal = self._calibration_tab.calibration_commit_snapshot()
        rec = self._recorded_tab.dsp_commit_snapshot()
        rt = self._realtime_tab.dsp_commit_snapshot()
        self._render_mismatch(rec, rt)
        self._render_calibration(cal)
        selected = self._selected_dsp_snapshot()
        self._summary.setPlainText(self._format_dsp_summary(selected))

    def _render_calibration(self, snapshot: CalibrationCommitSnapshot) -> None:
        if snapshot.variant_yaml is None:
            self._calibration_label.setStyleSheet("color: #e05a5a;")
            self._calibration_label.setText(
                f"Calibration variant: {snapshot.variant} (not available yet; compile calibration first)."
            )
            return
        exists = snapshot.variant_yaml.is_file()
        color = "#4caf50" if exists else "#e05a5a"
        self._calibration_label.setStyleSheet(f"color: {color};")
        self._calibration_label.setText(
            f"Calibration variant: {snapshot.variant} -> {snapshot.variant_yaml}"
            + ("" if exists else " (missing file)")
        )

    def _render_mismatch(self, recorded: DspCommitSnapshot, realtime: DspCommitSnapshot) -> None:
        if recorded == realtime:
            self._mismatch_label.setStyleSheet("color: #4caf50;")
            self._mismatch_label.setText("Recorded Data and Real-Time Audio DSP settings match.")
            return
        self._mismatch_label.setStyleSheet("color: #c9a227;")
        self._mismatch_label.setText(
            "Recorded Data and Real-Time Audio DSP settings differ. "
            "Choose which source should be committed."
        )

    def _format_dsp_summary(self, snapshot: DspCommitSnapshot) -> str:
        mode = snapshot.suppression_mode.upper()
        backend = snapshot.suppression_backend or "(unchanged)"
        suppressor = snapshot.suppressor
        binaural = snapshot.binaural
        return "\n".join(
            [
                f"Suppression mode request: {mode}",
                f"Suppression backend: {backend}",
                f"Suppression fade_ms: {suppressor.fade_ms:.1f}",
                f"Suppression activity_threshold: {suppressor.activity_threshold:.4f}",
                f"Suppression confidence_threshold: {suppressor.confidence_threshold:.2f}",
                f"Steering ambient_floor_linear: {suppressor.ambient_floor_linear:.3f}",
                f"Binaural enabled: {binaural.enabled}",
                f"Binaural backend: {binaural.backend or '(unchanged)'}",
                f"Binaural follow_steering: {binaural.follow_beamformer_steering}",
                f"Binaural azimuth_deg: {binaural.azimuth_deg:.1f}",
                f"Binaural elevation_deg: {binaural.elevation_deg:.1f}",
            ]
        )

    def _on_commit(self) -> None:
        cal = self._calibration_tab.calibration_commit_snapshot()
        if cal.variant_yaml is None:
            QMessageBox.warning(self, "No calibration artifacts", "Compile calibration first.")
            return
        if not cal.variant_yaml.is_file():
            QMessageBox.warning(
                self,
                "Missing calibration YAML",
                f"Selected variant file is missing:\n{cal.variant_yaml}",
            )
            return

        dsp = self._selected_dsp_snapshot()
        try:
            result = commit_runtime_config(
                DEFAULT_CONFIG_PATH,
                base_config=DEFAULT_CONFIG_PATH,
                calibration_src=cal.variant_yaml,
                common_eq_enabled=cal.common_eq_enabled,
                common_eq_sections=cal.common_eq_sections,
                dsp=dsp,
            )
        except Exception as exc:  # noqa: BLE001
            QMessageBox.critical(self, "Upload failed", str(exc))
            return

        self._apply_commit_result(result, cal.variant)
        self.runtimeConfigCommitted.emit(str(DEFAULT_CONFIG_PATH))
        self._refresh_summary()

    def _apply_commit_result(self, result: UploadCommitResult, variant: str) -> None:
        self._status.setStyleSheet("color: #4caf50;")
        self._status.setText(
            f"Committed {variant} to config/default.yaml. Recorded and Real-Time now point at that YAML; "
            "a running Real-Time stream is restarted automatically."
        )
        self._paths.setText(
            f"Config: {result.committed_config_path}\n"
            f"Calibration copy: {result.calibration_copy_path}\n"
            f"Backup: {result.backup_path}"
        )
        self._details.setPlainText(
            "\n".join(
                [
                    f"Suppression mode request: {result.suppression_mode.value}",
                    f"Resolved suppression.enabled: {result.resolved_suppression_enabled}",
                    f"Committed config path: {result.committed_config_path}",
                    f"Copied calibration path: {result.calibration_copy_path}",
                    f"Backup path: {result.backup_path}",
                ]
            )
        )
