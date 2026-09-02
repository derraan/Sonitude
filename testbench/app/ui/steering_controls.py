"""Steering dial plus typed azimuth and Directional↔Omni blend (0–100%)."""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QSlider,
    QVBoxLayout,
    QWidget,
)

from app.ui.secondary_note import apply_secondary_note
from app.ui.steering_dial import SteeringDial

_BLEND_MAX_WIDTH_DEG = 180.0


def blend_percent_to_width_deg(percent: int) -> float:
    return max(0.0, min(100, percent)) / 100.0 * _BLEND_MAX_WIDTH_DEG


def width_deg_to_blend_percent(width_deg: float) -> int:
    return int(round(max(0.0, min(_BLEND_MAX_WIDTH_DEG, width_deg)) / _BLEND_MAX_WIDTH_DEG * 100.0))


class SteeringControls(QWidget):
    azimuthChanged = Signal(float)
    blendChanged = Signal(float)  # width_deg for the C++ compatibility field

    def __init__(self, title: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._dial = SteeringDial()
        self._azimuth_spin = QDoubleSpinBox()
        self._azimuth_spin.setRange(-180.0, 180.0)
        self._azimuth_spin.setDecimals(0)
        self._azimuth_spin.setSuffix("°")
        self._azimuth_spin.setToolTip("Commanded azimuth. 0° is forward; positive is to the right.")
        self._azimuth_spin.setAccessibleName("Commanded steering azimuth")

        self._blend_slider = QSlider(Qt.Orientation.Horizontal)
        self._blend_slider.setRange(0, 100)
        self._blend_slider.setValue(0)
        self._blend_slider.setAccessibleName("Directional to omni blend")
        self._blend_label = QLabel("Directional ↔ Omni: 0% omni (fully directional)")

        self._dial.azimuthChanged.connect(self._on_dial_azimuth)
        self._azimuth_spin.valueChanged.connect(self._on_spin_azimuth)
        self._blend_slider.valueChanged.connect(self._on_blend_changed)

        box = QGroupBox(title)
        layout = QVBoxLayout(box)
        layout.addWidget(self._dial, alignment=Qt.AlignmentFlag.AlignHCenter)

        form = QFormLayout()
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        form.addRow("Azimuth:", self._azimuth_spin)
        layout.addLayout(form)

        blend_header = QHBoxLayout()
        blend_header.addWidget(QLabel("Directional"))
        blend_header.addStretch(1)
        blend_header.addWidget(QLabel("Omni"))
        layout.addLayout(blend_header)
        layout.addWidget(self._blend_label)
        layout.addWidget(self._blend_slider)
        blend_note = QLabel(
            "MVDR beamformer: changes which direction is emphasized in the mono "
            "tap. Does not pan L/R — headphone spatial image comes from the binaural renderer."
        )
        apply_secondary_note(blend_note)
        layout.addWidget(blend_note)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(box)

    def commanded_azimuth_deg(self) -> float:
        return self._dial.commanded_azimuth_deg()

    def width_deg(self) -> float:
        return blend_percent_to_width_deg(self._blend_slider.value())

    def set_estimated_azimuth_deg(self, azimuth_deg: float | None) -> None:
        self._dial.set_estimated_azimuth_deg(azimuth_deg)

    def _on_dial_azimuth(self, azimuth_deg: float) -> None:
        self._azimuth_spin.blockSignals(True)
        self._azimuth_spin.setValue(azimuth_deg)
        self._azimuth_spin.blockSignals(False)
        self.azimuthChanged.emit(azimuth_deg)

    def _on_spin_azimuth(self, azimuth_deg: float) -> None:
        self._dial.set_commanded_azimuth_deg(azimuth_deg, emit=False)
        self.azimuthChanged.emit(azimuth_deg)

    def _on_blend_changed(self, percent: int) -> None:
        width_deg = blend_percent_to_width_deg(percent)
        if percent <= 0:
            descriptor = "fully directional"
        elif percent >= 100:
            descriptor = "fully omnidirectional mix"
        else:
            descriptor = "blended"
        self._blend_label.setText(f"Directional ↔ Omni: {percent}% omni ({descriptor})")
        self._dial.set_width_deg(width_deg)
        self.blendChanged.emit(width_deg)
