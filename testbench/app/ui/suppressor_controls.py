"""Live conservative suppressor controls for Real-Time and Recorded preview tabs."""

from __future__ import annotations

import math

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QLabel,
    QVBoxLayout,
    QWidget,
)

from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.processing.suppression import SuppressionMode
from app.storage.models import SuppressorRequest
from app.ui.secondary_note import apply_secondary_note


def _linear_to_db(linear: float) -> float:
    if linear <= 0.0:
        return -120.0
    return 20.0 * math.log10(linear)


class SuppressorControls(QWidget):
    changed = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)

        self._mode = QComboBox()
        self._mode.addItem("AUTO (use YAML)", userData=SuppressionMode.AUTO.value)
        self._mode.addItem("ON (force on)", userData=SuppressionMode.ON.value)
        self._mode.addItem("OFF (force off, overrides YAML)", userData=SuppressionMode.OFF.value)

        self._focus = QComboBox()
        self._focus.addItem("Focus active (duck when gates pass)", userData=True)
        self._focus.addItem("Focus off (bypass ducking)", userData=False)

        self._ambient_floor = QDoubleSpinBox()
        self._ambient_floor.setRange(0.0, 1.0)
        self._ambient_floor.setSingleStep(0.01)
        self._ambient_floor.setDecimals(3)
        self._ambient_floor.setToolTip(
            "Minimum gain when ducking is active. Lower = deeper attenuation of off-beam content."
        )
        self._ambient_floor_db = QLabel("")

        self._fade_ms = QDoubleSpinBox()
        self._fade_ms.setRange(1.0, 1000.0)
        self._fade_ms.setSuffix(" ms")
        self._fade_ms.setDecimals(0)
        self._fade_ms.setToolTip("Gain ramp time toward the ambient floor or back to unity.")

        self._activity = QDoubleSpinBox()
        self._activity.setRange(0.0, 1.0)
        self._activity.setSingleStep(0.005)
        self._activity.setDecimals(4)
        self._activity.setToolTip(
            "Envelope must reach this level before ducking can engage. Lower = more sensitive."
        )

        self._confidence_threshold = QDoubleSpinBox()
        self._confidence_threshold.setRange(0.0, 1.0)
        self._confidence_threshold.setSingleStep(0.05)
        self._confidence_threshold.setDecimals(2)
        self._confidence_threshold.setToolTip(
            "Live confidence must be at or above this value for ducking to engage."
        )

        self._confidence = QDoubleSpinBox()
        self._confidence.setRange(0.0, 1.0)
        self._confidence.setSingleStep(0.05)
        self._confidence.setDecimals(2)
        self._confidence.setToolTip(
            "Simulated focus confidence sent each block. Lower the confidence threshold or "
            "raise this to make ducking easier to trigger."
        )

        self._env_attack = QDoubleSpinBox()
        self._env_attack.setRange(0.001, 1.0)
        self._env_attack.setSingleStep(0.01)
        self._env_attack.setDecimals(3)
        self._env_attack.setToolTip(
            "How quickly the level envelope follows rising signal. Higher = faster, more responsive."
        )

        self._env_release = QDoubleSpinBox()
        self._env_release.setRange(0.0001, 1.0)
        self._env_release.setSingleStep(0.005)
        self._env_release.setDecimals(4)
        self._env_release.setToolTip(
            "How quickly the envelope falls. Lower = slower decay, less chatter between words."
        )

        self._note = QLabel("")
        apply_secondary_note(self._note)

        box = QGroupBox("Conservative suppressor (live)")
        form = QFormLayout(box)
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        form.addRow("Mode:", self._mode)
        form.addRow("Focus:", self._focus)
        form.addRow("Ambient floor (linear):", self._ambient_floor)
        form.addRow("Ambient floor (dB):", self._ambient_floor_db)
        form.addRow("Fade time:", self._fade_ms)
        form.addRow("Activity threshold:", self._activity)
        form.addRow("Confidence threshold:", self._confidence_threshold)
        form.addRow("Live confidence:", self._confidence)
        form.addRow("Envelope attack:", self._env_attack)
        form.addRow("Envelope release:", self._env_release)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(box)
        layout.addWidget(self._note)

        self._load_yaml_defaults()
        self._update_ambient_floor_db()

        for widget in (
            self._mode,
            self._focus,
            self._ambient_floor,
            self._fade_ms,
            self._activity,
            self._confidence_threshold,
            self._confidence,
            self._env_attack,
            self._env_release,
        ):
            if isinstance(widget, QComboBox):
                widget.currentIndexChanged.connect(lambda _i: self.changed.emit())
            else:
                widget.valueChanged.connect(self._on_value_changed)

    def suppression_mode(self) -> str:
        return self._mode.currentData() or SuppressionMode.AUTO.value

    def request(self) -> SuppressorRequest:
        return SuppressorRequest(
            ambient_floor_linear=self._ambient_floor.value(),
            fade_ms=self._fade_ms.value(),
            activity_threshold=self._activity.value(),
            confidence_threshold=self._confidence_threshold.value(),
            envelope_attack_coeff=self._env_attack.value(),
            envelope_release_coeff=self._env_release.value(),
            confidence=self._confidence.value(),
            focus_active=bool(self._focus.currentData()),
        )

    def _on_value_changed(self, _value: float) -> None:
        if self.sender() is self._ambient_floor:
            self._update_ambient_floor_db()
        self.changed.emit()

    def _update_ambient_floor_db(self) -> None:
        self._ambient_floor_db.setText(f"{_linear_to_db(self._ambient_floor.value()):.1f} dBFS rel.")

    def _load_yaml_defaults(self) -> None:
        try:
            summary = read_runtime_config_summary(DEFAULT_CONFIG_PATH)
            s = summary.suppression
            if summary.suppression_enabled:
                self._mode.setCurrentIndex(0)
            self._ambient_floor.setValue(s.ambient_floor_linear)
            self._fade_ms.setValue(s.fade_ms)
            self._activity.setValue(s.activity_threshold)
            self._confidence_threshold.setValue(s.confidence_threshold)
            self._env_attack.setValue(s.envelope_attack_coeff)
            self._env_release.setValue(s.envelope_release_coeff)
        except Exception:  # noqa: BLE001 - keep built-in defaults
            pass
        self._note.setText(
            "Broadband gain gate on the beamformed mono: when focus is active, confidence and "
            "activity gates pass, gain ramps toward the ambient floor (−12 dB at 0.25). "
            "This is not frequency-selective — use steering azimuth and Directional↔Omni blend "
            "for directional sharpness. Lower activity threshold, lower confidence threshold, "
            "lower ambient floor, or faster fade/envelope attack for more aggressive ducking."
        )
