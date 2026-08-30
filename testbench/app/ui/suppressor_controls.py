"""Suppression mode, backend, and live conservative knobs for both test-bench tabs."""

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
from app.processing.capabilities import ToolCapabilities, preferred_suppression_backend
from app.processing.suppression import SuppressionMode
from app.storage.models import SuppressorRequest
from app.ui.secondary_note import apply_secondary_note

BACKEND_LABELS = {
    "conservative": "conservative (broadband gate)",
    "spectral": "spectral (STFT Wiener)",
}


def _linear_to_db(linear: float) -> float:
    if linear <= 0.0:
        return -120.0
    return 20.0 * math.log10(linear)


class SuppressorControls(QWidget):
    changed = Signal()
    backendChanged = Signal()

    def __init__(self, capabilities: ToolCapabilities, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._capabilities = capabilities

        self._mode = QComboBox()
        self._mode.addItem("AUTO (use YAML)", userData=SuppressionMode.AUTO.value)
        self._mode.addItem("ON (force on)", userData=SuppressionMode.ON.value)
        self._mode.addItem("OFF (force off, overrides YAML)", userData=SuppressionMode.OFF.value)

        self._backend = QComboBox()
        self._backend.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self._backend.setMinimumContentsLength(16)
        self._backend.setToolTip(
            "C++ suppressor backend. Spectral NS shares the MVDR 128/32 hop "
            "(no extra STFT delay). Requires a rebuilt sonitude_wav_replay / "
            "sonitude_stream_process."
        )

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

        self._conservative_note = QLabel("")
        apply_secondary_note(self._conservative_note)
        self._note = QLabel("")
        apply_secondary_note(self._note)

        box = QGroupBox("Suppression (C++ backend)")
        form = QFormLayout(box)
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        form.addRow("Mode:", self._mode)
        form.addRow("Backend:", self._backend)
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
        layout.addWidget(self._conservative_note)
        layout.addWidget(self._note)

        self._load_yaml_defaults()
        self._populate_backends()
        self._update_ambient_floor_db()
        self._sync_backend_widgets()

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
        self._mode.currentIndexChanged.connect(lambda _i: self.changed.emit())
        self._backend.currentIndexChanged.connect(self._on_backend_changed)

    def suppression_mode(self) -> str:
        return self._mode.currentData() or SuppressionMode.AUTO.value

    def suppression_backend(self) -> str | None:
        backend = self._backend.currentData()
        return backend if isinstance(backend, str) else None

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

    def _on_backend_changed(self, _index: int) -> None:
        self._sync_backend_widgets()
        self.changed.emit()
        self.backendChanged.emit()

    def _on_value_changed(self, _value: float) -> None:
        if self.sender() is self._ambient_floor:
            self._update_ambient_floor_db()
        self.changed.emit()

    def _update_ambient_floor_db(self) -> None:
        self._ambient_floor_db.setText(f"{_linear_to_db(self._ambient_floor.value()):.1f} dBFS rel.")

    def _selected_backend(self) -> str | None:
        backend = self._backend.currentData()
        return backend if isinstance(backend, str) else None

    def _sync_backend_widgets(self) -> None:
        backend = self._selected_backend()
        conservative = backend == "conservative"
        spectral = backend == "spectral"
        enabled = self._backend.isEnabled()
        self._focus.setEnabled(enabled and (conservative or spectral))
        self._confidence_threshold.setEnabled(enabled and (conservative or spectral))
        self._confidence.setEnabled(enabled and (conservative or spectral))
        for widget in (
            self._ambient_floor,
            self._fade_ms,
            self._activity,
            self._env_attack,
            self._env_release,
        ):
            widget.setEnabled(enabled and conservative)
        self._ambient_floor_db.setEnabled(enabled and conservative)
        if conservative:
            self._conservative_note.setText(
                "Broadband gain gate on beamformed mono: when focus is active and gates pass, "
                "gain ramps toward the ambient floor. Not frequency-selective."
            )
        elif spectral:
            self._conservative_note.setText(
                "Spectral backend uses focus, live confidence, and the confidence threshold. "
                "Ambient floor, fade, activity, and envelope knobs are conservative-only and "
                "are ignored. FFT/hop/gain floor come from YAML suppression.spectral.*."
            )
        else:
            self._conservative_note.setText("Backend off — beamformed mono passes through unchanged.")

    def _populate_backends(self) -> None:
        caps = self._capabilities.suppression
        self._backend.clear()
        if not self._capabilities.queried or not caps.backends:
            for name in ("conservative", "spectral"):
                label = BACKEND_LABELS.get(name, name)
                self._backend.addItem(label, userData=name)
            self._backend.setEnabled(False)
            self._note.setText(
                "Rebuild sonitude_wav_replay and sonitude_stream_process, then restart the GUI "
                "so --capabilities lists suppression backends."
            )
            return

        for name in caps.backends:
            label = BACKEND_LABELS.get(name, name)
            self._backend.addItem(label, userData=name)

        yaml_backend: str | None = None
        try:
            yaml_backend = read_runtime_config_summary(DEFAULT_CONFIG_PATH).suppression.backend
        except Exception:  # noqa: BLE001
            yaml_backend = None

        chosen = preferred_suppression_backend(caps.backends, yaml_backend)
        if chosen is not None:
            index = self._backend.findData(chosen)
            if index >= 0:
                self._backend.setCurrentIndex(index)

        self._backend.setEnabled(True)
        binary = (
            f" Binary: {self._capabilities.binary_path}."
            if self._capabilities.binary_path
            else ""
        )
        self._note.setText(
            f"Backend selection maps to CLI --suppression-backend.{binary}"
        )

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
