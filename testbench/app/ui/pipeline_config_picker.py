"""Runtime YAML + calibration YAML pickers for Recorded and Real-Time tabs."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QWidget,
)

from app.config_reader import DEFAULT_CONFIG_PATH
from app.controller.calibration_controller import write_dsp_runtime_overlay
from app.controller.runtime_yaml_catalog import (
    calibration_path_from_runtime,
    list_calibration_yaml_files,
    list_runtime_yaml_files,
)
from app.storage.result_store import DEFAULT_DATA_ROOT
from app.ui.layout_persist import settings
from app.ui.secondary_note import apply_secondary_note

KEY_RUNTIME = "pipeline/runtime_yaml"
KEY_CALIBRATION = "pipeline/calibration_yaml"


def _compact_combo(combo: QComboBox) -> None:
    combo.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
    combo.setMinimumContentsLength(22)


def _label_for(path: Path) -> str:
    try:
        return f"{path.parent.name}/{path.name}"
    except OSError:
        return path.name


class PipelineConfigPicker(QGroupBox):
    selectionChanged = Signal()

    def __init__(self, session_yaml: Path, parent: QWidget | None = None) -> None:
        super().__init__("Pipeline YAML (what C++ will load)", parent)
        self._session_yaml = Path(session_yaml)
        self._runtime_combo = QComboBox()
        _compact_combo(self._runtime_combo)
        self._cal_combo = QComboBox()
        _compact_combo(self._cal_combo)
        refresh = QPushButton("Refresh lists")
        refresh.clicked.connect(self.refresh_lists)
        self._status = QLabel("—")
        self._status.setWordWrap(True)
        self._status.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        apply_secondary_note(self._status)
        note = QLabel(
            "Compile + Apply on Calibration sets these automatically. "
            "Upload later copies the selection into config/default.yaml."
        )
        apply_secondary_note(note)

        form = QFormLayout()
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        runtime_row = QWidget()
        runtime_layout = QHBoxLayout(runtime_row)
        runtime_layout.setContentsMargins(0, 0, 0, 0)
        runtime_layout.addWidget(self._runtime_combo, stretch=1)
        runtime_layout.addWidget(refresh)
        form.addRow("Runtime YAML:", runtime_row)
        form.addRow("Calibration YAML:", self._cal_combo)
        form.addRow(self._status)
        form.addRow(note)
        self.setLayout(form)

        self._runtime_combo.currentIndexChanged.connect(self._on_runtime_changed)
        self._cal_combo.currentIndexChanged.connect(self._on_cal_changed)
        self.refresh_lists(restore_settings=True)

    def refresh_lists(self, restore_settings: bool = False) -> None:
        runtime_target = self.runtime_path()
        cal_target = self.calibration_path()
        if restore_settings:
            stored_rt = settings().value(KEY_RUNTIME)
            stored_cal = settings().value(KEY_CALIBRATION)
            if isinstance(stored_rt, str) and Path(stored_rt).is_file():
                runtime_target = Path(stored_rt)
            if isinstance(stored_cal, str) and Path(stored_cal).is_file():
                cal_target = Path(stored_cal)

        self._runtime_combo.blockSignals(True)
        self._cal_combo.blockSignals(True)
        self._runtime_combo.clear()
        for path in list_runtime_yaml_files():
            self._runtime_combo.addItem(_label_for(path), userData=path)
            self._runtime_combo.setItemData(
                self._runtime_combo.count() - 1, str(path), Qt.ItemDataRole.ToolTipRole
            )
        if self._runtime_combo.count() == 0:
            self._runtime_combo.addItem(_label_for(DEFAULT_CONFIG_PATH), userData=DEFAULT_CONFIG_PATH)
        self._cal_combo.clear()
        for path in list_calibration_yaml_files():
            self._cal_combo.addItem(_label_for(path), userData=path)
            self._cal_combo.setItemData(
                self._cal_combo.count() - 1, str(path), Qt.ItemDataRole.ToolTipRole
            )
        self._select_path(self._runtime_combo, runtime_target)
        inferred = calibration_path_from_runtime(self.runtime_path()) if cal_target is None else cal_target
        self._select_path(self._cal_combo, inferred)
        self._runtime_combo.blockSignals(False)
        self._cal_combo.blockSignals(False)
        self._update_status()

    def set_selection(self, runtime_yaml: Path, calibration_yaml: Path | None = None) -> None:
        self.refresh_lists(restore_settings=False)
        self._runtime_combo.blockSignals(True)
        self._cal_combo.blockSignals(True)
        self._ensure_combo_has(self._runtime_combo, Path(runtime_yaml))
        self._select_path(self._runtime_combo, Path(runtime_yaml))
        cal = Path(calibration_yaml) if calibration_yaml is not None else calibration_path_from_runtime(Path(runtime_yaml))
        if cal is not None:
            self._ensure_combo_has(self._cal_combo, cal)
            self._select_path(self._cal_combo, cal)
        self._runtime_combo.blockSignals(False)
        self._cal_combo.blockSignals(False)
        self._persist()
        self._update_status()
        self.selectionChanged.emit()

    def runtime_path(self) -> Path:
        data = self._runtime_combo.currentData()
        return Path(data) if data else DEFAULT_CONFIG_PATH

    def calibration_path(self) -> Path | None:
        data = self._cal_combo.currentData()
        return Path(data) if data else None

    def materialize_config(self) -> Path:
        runtime = self.runtime_path()
        cal = self.calibration_path()
        self._session_yaml.parent.mkdir(parents=True, exist_ok=True)
        write_dsp_runtime_overlay(
            self._session_yaml,
            calibration_yaml=cal if cal is not None and cal.is_file() else None,
            base_config=runtime,
        )
        return self._session_yaml

    def _on_runtime_changed(self, _index: int) -> None:
        inferred = calibration_path_from_runtime(self.runtime_path())
        if inferred is not None:
            self._cal_combo.blockSignals(True)
            self._select_path(self._cal_combo, inferred)
            self._cal_combo.blockSignals(False)
        self._persist()
        self._update_status()
        self.selectionChanged.emit()

    def _on_cal_changed(self, _index: int) -> None:
        self._persist()
        self._update_status()
        self.selectionChanged.emit()

    def _persist(self) -> None:
        settings().setValue(KEY_RUNTIME, str(self.runtime_path()))
        cal = self.calibration_path()
        if cal is not None:
            settings().setValue(KEY_CALIBRATION, str(cal))

    def _update_status(self) -> None:
        runtime = self.runtime_path()
        cal = self.calibration_path()
        cal_name = cal.name if cal is not None else "(none)"
        self._status.setText(f"Active: {runtime.name}  +  {cal_name}")
        self._status.setToolTip(f"{runtime}\n{cal or ''}")
        self._runtime_combo.setToolTip(str(runtime))
        if cal is not None:
            self._cal_combo.setToolTip(str(cal))

    @staticmethod
    def _select_path(combo: QComboBox, path: Path | None) -> None:
        if path is None:
            return
        target = path.resolve()
        for index in range(combo.count()):
            data = combo.itemData(index)
            if data is not None and Path(data).resolve() == target:
                combo.setCurrentIndex(index)
                return

    @staticmethod
    def _ensure_combo_has(combo: QComboBox, path: Path) -> None:
        path = path.resolve()
        if not path.is_file():
            return
        for index in range(combo.count()):
            data = combo.itemData(index)
            if data is not None and Path(data).resolve() == path:
                return
        combo.insertItem(0, _label_for(path), userData=path)
        combo.setItemData(0, str(path), Qt.ItemDataRole.ToolTipRole)


def recorded_session_yaml() -> Path:
    return DEFAULT_DATA_ROOT / "session_recorded_runtime.yaml"


def realtime_session_yaml() -> Path:
    return DEFAULT_DATA_ROOT / "session_realtime_runtime.yaml"
