"""Suppression mode, backend, and live conservative knobs for both test-bench tabs."""

from __future__ import annotations

import math

import yaml
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
    yamlReloadNeeded = Signal()

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

        self._spectral_gain_floor = QDoubleSpinBox()
        self._spectral_gain_floor.setRange(-80.0, 0.0)
        self._spectral_gain_floor.setSingleStep(1.0)
        self._spectral_gain_floor.setDecimals(1)
        self._spectral_gain_floor.setSuffix(" dB")
        self._spectral_gain_floor.setValue(-12.0)
        self._spectral_gain_floor.setToolTip(
            "Minimum Wiener gain per bin. Lower (more negative) = deeper noise attenuation."
        )

        self._spectral_protect = QDoubleSpinBox()
        self._spectral_protect.setRange(1.0, 16.0)
        self._spectral_protect.setSingleStep(0.5)
        self._spectral_protect.setDecimals(1)
        self._spectral_protect.setValue(4.0)
        self._spectral_protect.setToolTip(
            "Target/guard power ratio for speech protection. Lower = more bins treated as noise."
        )

        self._spectral_overestimate = QDoubleSpinBox()
        self._spectral_overestimate.setRange(1.0, 8.0)
        self._spectral_overestimate.setSingleStep(0.25)
        self._spectral_overestimate.setDecimals(2)
        self._spectral_overestimate.setValue(2.0)
        self._spectral_overestimate.setToolTip(
            "Noise power multiplier in the Wiener denominator. Higher = more aggressive suppression."
        )

        self._spectral_tonal = QDoubleSpinBox()
        self._spectral_tonal.setRange(2.0, 16.0)
        self._spectral_tonal.setSingleStep(0.5)
        self._spectral_tonal.setDecimals(1)
        self._spectral_tonal.setValue(6.0)
        self._spectral_tonal.setToolTip(
            "Bins above median×this are treated as tonal and skipped for noise learning."
        )

        self._spectral_noise_rise = QDoubleSpinBox()
        self._spectral_noise_rise.setRange(50.0, 4000.0)
        self._spectral_noise_rise.setSingleStep(50.0)
        self._spectral_noise_rise.setDecimals(0)
        self._spectral_noise_rise.setSuffix(" ms")
        self._spectral_noise_rise.setValue(480.0)
        self._spectral_noise_rise.setToolTip(
            "How quickly the noise floor rises when level increases. Lower = faster noise tracking."
        )

        self._amplitude_range_bias = QCheckBox("Amplitude range bias (near-mic speech band)")
        self._amplitude_range_bias.setChecked(True)
        self._amplitude_range_bias.setToolTip(
            "When the closest mic dominates 300–4 kHz, pull speech-band Wiener gain toward 1 "
            "and hold other bins at the gain floor. Written to YAML; restart the stream to apply."
        )

        self._speech_low = QDoubleSpinBox()
        self._speech_low.setRange(80.0, 2000.0)
        self._speech_low.setSingleStep(20.0)
        self._speech_low.setDecimals(0)
        self._speech_low.setSuffix(" Hz")
        self._speech_low.setValue(300.0)
        self._speech_low.setToolTip("Low edge of the proximity speech band.")

        self._speech_high = QDoubleSpinBox()
        self._speech_high.setRange(1000.0, 8000.0)
        self._speech_high.setSingleStep(100.0)
        self._speech_high.setDecimals(0)
        self._speech_high.setSuffix(" Hz")
        self._speech_high.setValue(4000.0)
        self._speech_high.setToolTip("High edge of the proximity speech band.")

        self._near_dominance = QDoubleSpinBox()
        self._near_dominance.setRange(1.05, 8.0)
        self._near_dominance.setSingleStep(0.05)
        self._near_dominance.setDecimals(2)
        self._near_dominance.setValue(1.4)
        self._near_dominance.setToolTip(
            "max/mean mic amplitude in the speech band that maps to proximity=1."
        )

        self._spectral_widgets = (
            self._spectral_gain_floor,
            self._spectral_protect,
            self._spectral_overestimate,
            self._spectral_tonal,
            self._spectral_noise_rise,
            self._speech_low,
            self._speech_high,
            self._near_dominance,
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

        self._spectral_box = QGroupBox("Spectral noise suppression tuning")
        spectral_form = QFormLayout(self._spectral_box)
        spectral_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        spectral_form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        spectral_form.addRow("Gain floor:", self._spectral_gain_floor)
        spectral_form.addRow("Speech protect ratio:", self._spectral_protect)
        spectral_form.addRow("Noise overestimate:", self._spectral_overestimate)
        spectral_form.addRow("Tonal guard:", self._spectral_tonal)
        spectral_form.addRow("Noise adapt time:", self._spectral_noise_rise)
        spectral_form.addRow(self._amplitude_range_bias)
        spectral_form.addRow("Speech band low:", self._speech_low)
        spectral_form.addRow("Speech band high:", self._speech_high)
        spectral_form.addRow("Near dominance ratio:", self._near_dominance)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(box)
        layout.addWidget(self._spectral_box)
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
            *self._spectral_widgets,
        ):
            if isinstance(widget, QComboBox):
                widget.currentIndexChanged.connect(lambda _i: self.changed.emit())
            else:
                widget.valueChanged.connect(self._on_value_changed)
        self._mode.currentIndexChanged.connect(lambda _i: self.changed.emit())
        self._backend.currentIndexChanged.connect(self._on_backend_changed)
        self._amplitude_range_bias.toggled.connect(self._on_yaml_knob_changed)
        for widget in (self._speech_low, self._speech_high, self._near_dominance):
            widget.valueChanged.connect(self._on_yaml_knob_changed)

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
            spectral_gain_floor_db=self._spectral_gain_floor.value(),
            spectral_protect_ratio=self._spectral_protect.value(),
            spectral_noise_overestimate=self._spectral_overestimate.value(),
            spectral_tonal_ratio=self._spectral_tonal.value(),
            spectral_noise_rise_ms=self._spectral_noise_rise.value(),
            amplitude_range_bias=self._amplitude_range_bias.isChecked(),
            speech_low_hz=self._speech_low.value(),
            speech_high_hz=self._speech_high.value(),
            near_dominance_ratio=self._near_dominance.value(),
        )

    def _on_backend_changed(self, _index: int) -> None:
        self._sync_backend_widgets()
        self.changed.emit()
        self.backendChanged.emit()

    def _on_yaml_knob_changed(self, *_args) -> None:
        self.yamlReloadNeeded.emit()

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
        spectral_enabled = enabled and spectral
        self._spectral_box.setEnabled(spectral_enabled)
        for widget in self._spectral_widgets:
            widget.setEnabled(spectral_enabled)
        self._amplitude_range_bias.setEnabled(spectral_enabled)
        if conservative:
            self._conservative_note.setText(
                "Broadband gain gate on beamformed mono: when focus is active and gates pass, "
                "gain ramps toward the ambient floor. Not frequency-selective."
            )
        elif spectral:
            self._conservative_note.setText(
                "Spectral Wiener: live knobs (floor / protect / overestimate / tonal / noise rise) "
                "apply per block. Amplitude range bias and speech-band edges are YAML-only and "
                "restart the stream. Set Mode to ON to hear suppression."
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
            with open(DEFAULT_CONFIG_PATH, encoding="utf-8") as handle:
                raw = yaml.safe_load(handle) or {}
            spectral = (raw.get("suppression") or {}).get("spectral") or {}
            if isinstance(spectral, dict):
                self._spectral_gain_floor.setValue(float(spectral.get("gain_floor_db", -12.0)))
                if "amplitude_range_bias" in spectral:
                    self._amplitude_range_bias.setChecked(bool(spectral["amplitude_range_bias"]))
                if spectral.get("speech_low_hz") is not None:
                    self._speech_low.setValue(float(spectral["speech_low_hz"]))
                if spectral.get("speech_high_hz") is not None:
                    self._speech_high.setValue(float(spectral["speech_high_hz"]))
                if spectral.get("near_dominance_ratio") is not None:
                    self._near_dominance.setValue(float(spectral["near_dominance_ratio"]))
        except Exception:  # noqa: BLE001 - keep built-in defaults
            pass
