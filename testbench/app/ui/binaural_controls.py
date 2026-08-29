"""Capability-gated binaural controls shared by Recorded and Real-Time tabs."""

from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QLabel,
    QVBoxLayout,
    QWidget,
)

from app.processing.capabilities import ToolCapabilities
from app.storage.models import BinauralRequest
from app.ui.secondary_note import apply_secondary_note


class BinauralControls(QWidget):
    changed = Signal()

    def __init__(self, capabilities: ToolCapabilities, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._capabilities = capabilities

        box = QGroupBox("Binaural renderer (C++ backend, capability-gated)")
        self._enable = QCheckBox("Binaural enabled")
        self._backend = QComboBox()
        self._backend.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self._backend.setMinimumContentsLength(16)
        self._follow = QCheckBox("Follow effective beamformer steering")
        self._follow.setChecked(True)
        self._azimuth = QDoubleSpinBox()
        self._azimuth.setRange(-180.0, 180.0)
        self._azimuth.setSuffix("°")
        self._elevation = QDoubleSpinBox()
        self._elevation.setRange(-90.0, 90.0)
        self._elevation.setSuffix("°")
        self._note = QLabel("")
        apply_secondary_note(self._note)

        form = QFormLayout()
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        form.addRow(self._enable)
        form.addRow("Renderer backend:", self._backend)
        form.addRow(self._follow)
        form.addRow("Azimuth:", self._azimuth)
        form.addRow("Elevation:", self._elevation)

        layout = QVBoxLayout(box)
        layout.addLayout(form)
        layout.addWidget(self._note)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(box)

        self._populate()
        self._enable.toggled.connect(lambda _c: self.changed.emit())
        self._backend.currentIndexChanged.connect(lambda _i: self.changed.emit())
        self._follow.toggled.connect(lambda _c: self.changed.emit())
        self._azimuth.valueChanged.connect(lambda _v: self.changed.emit())
        self._elevation.valueChanged.connect(lambda _v: self.changed.emit())

    def request(self) -> BinauralRequest:
        backend = self._backend.currentData()
        return BinauralRequest(
            enabled=self._enable.isChecked() and self._enable.isEnabled(),
            backend=backend if isinstance(backend, str) else None,
            azimuth_deg=self._azimuth.value(),
            elevation_deg=self._elevation.value(),
            follow_beamformer_steering=self._follow.isChecked(),
        )

    def _populate(self) -> None:
        caps = self._capabilities.binaural
        self._backend.clear()
        usable = bool(caps.available and self._capabilities.queried and caps.backends)
        for widget in (self._enable, self._backend, self._follow, self._azimuth, self._elevation):
            widget.setEnabled(usable)
        if not self._capabilities.queried:
            self._note.setText(
                "C++ binaural capabilities were not reported (binary missing or older than this protocol). "
                "The rest of the test bench remains usable."
            )
            return
        if not usable:
            self._note.setText("This C++ build reports binaural as unavailable.")
            return
        for name in caps.backends:
            self._backend.addItem(name, userData=name)
        unavailable = ", ".join(caps.unavailable_backends) or "none"
        self._note.setText(f"{caps.note} Unavailable backends (not offered): {unavailable}.")
