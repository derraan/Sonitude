"""Live MVDR beamformer tuning knobs (protocol v4)."""

from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QLabel,
    QVBoxLayout,
    QWidget,
)

from app.ui.secondary_note import apply_secondary_note


class BeamformerControls(QWidget):
    changed = Signal()

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

        self._note = QLabel(
            "MVDR runs before suppression. These knobs apply live during streaming preview."
        )
        apply_secondary_note(self._note)

        box = QGroupBox("MVDR beamformer tuning")
        form = QFormLayout(box)
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        form.addRow("Directivity (max WNG):", self._max_wn_gain)
        form.addRow("Adaptation time:", self._cov_tau_ms)
        form.addRow("Diagonal loading:", self._diag_load)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(box)
        layout.addWidget(self._note)

        for widget in (self._max_wn_gain, self._cov_tau_ms, self._diag_load):
            widget.valueChanged.connect(lambda _v: self.changed.emit())

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
