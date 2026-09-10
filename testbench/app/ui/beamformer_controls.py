"""Live MVDR beamformer tuning knobs (protocol v4)."""

from __future__ import annotations

import yaml
from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QLabel,
    QVBoxLayout,
    QWidget,
)

from app.config_reader import DEFAULT_CONFIG_PATH
from app.ui.secondary_note import apply_secondary_note


class BeamformerControls(QWidget):
    changed = Signal()
    yamlReloadNeeded = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)

        self._max_wn_gain = QDoubleSpinBox()
        self._max_wn_gain.setRange(1.0, 32.0)
        self._max_wn_gain.setSingleStep(0.5)
        self._max_wn_gain.setDecimals(1)
        self._max_wn_gain.setValue(4.0)
        self._max_wn_gain.setToolTip(
            "Maximum white-noise gain before MVDR falls back to delay-and-sum. "
            "Higher = stronger nulls and more off-axis rejection (try 6–12 if steering feels weak)."
        )

        self._cov_tau_ms = QDoubleSpinBox()
        self._cov_tau_ms.setRange(10.0, 2000.0)
        self._cov_tau_ms.setSingleStep(10.0)
        self._cov_tau_ms.setDecimals(0)
        self._cov_tau_ms.setSuffix(" ms")
        self._cov_tau_ms.setValue(80.0)
        self._cov_tau_ms.setToolTip(
            "Covariance adaptation time constant. Lower = faster tracking of moving interferers."
        )

        self._diag_load = QDoubleSpinBox()
        self._diag_load.setRange(0.001, 1.0)
        self._diag_load.setSingleStep(0.01)
        self._diag_load.setDecimals(3)
        self._diag_load.setValue(0.08)
        self._diag_load.setToolTip(
            "Diagonal loading for matrix stability. Lower = sharper beams but more sensitive to errors."
        )

        self._source_distance = QDoubleSpinBox()
        self._source_distance.setRange(0.05, 5.0)
        self._source_distance.setSingleStep(0.05)
        self._source_distance.setDecimals(2)
        self._source_distance.setSuffix(" m")
        self._source_distance.setValue(0.45)
        self._source_distance.setToolTip(
            "Spherical near-field look distance. Written into session YAML and requires a stream restart."
        )

        self._note = QLabel(
            "Near-field MVDR. WNG / covariance / loading apply live. "
            "Look distance is YAML-only and restarts the stream."
        )
        apply_secondary_note(self._note)

        box = QGroupBox("Near-field MVDR beamformer")
        form = QFormLayout(box)
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        form.addRow("Look distance:", self._source_distance)
        form.addRow("Directivity (max WNG):", self._max_wn_gain)
        form.addRow("Adaptation time:", self._cov_tau_ms)
        form.addRow("Diagonal loading:", self._diag_load)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(box)
        layout.addWidget(self._note)

        self._load_yaml_defaults()

        for widget in (self._max_wn_gain, self._cov_tau_ms, self._diag_load):
            widget.valueChanged.connect(lambda _v: self.changed.emit())
        self._source_distance.valueChanged.connect(self._on_distance_changed)

    def source_distance_m(self) -> float:
        return self._source_distance.value()

    def mvdr_max_wn_gain(self) -> float:
        return self._max_wn_gain.value()

    def mvdr_cov_tau_ms(self) -> float:
        return self._cov_tau_ms.value()

    def mvdr_diag_load(self) -> float:
        return self._diag_load.value()

    def apply_to_request(self, request) -> None:
        request.mvdr_max_wn_gain = self.mvdr_max_wn_gain()
        request.mvdr_cov_tau_ms = self.mvdr_cov_tau_ms()
        request.mvdr_diag_load = self.mvdr_diag_load()

    def _on_distance_changed(self, _value: float) -> None:
        self.yamlReloadNeeded.emit()

    def _load_yaml_defaults(self) -> None:
        try:
            with open(DEFAULT_CONFIG_PATH, encoding="utf-8") as handle:
                raw = yaml.safe_load(handle) or {}
            steering = raw.get("steering") or {}
            if isinstance(steering, dict) and steering.get("source_distance_m") is not None:
                self._source_distance.setValue(float(steering["source_distance_m"]))
        except Exception:  # noqa: BLE001
            pass
