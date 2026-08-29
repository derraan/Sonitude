"""Displays the metrics.json produced for a test result: noise suppression,
approximate SII, residual energy, and steering (commanded-only unless/until
an estimator exists). Every value that is an estimate rather than a precise
measurement is labeled as such, per testbench/README.md's metric definitions.
"""

from __future__ import annotations

from PySide6.QtWidgets import QFormLayout, QGroupBox, QLabel, QVBoxLayout, QWidget


def _metric_label(value_text: str, method: str | None = None) -> str:
    return f"{value_text}  ({method})" if method else value_text


class MetricsPanel(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)

        self._noise_labels = self._build_group(
            "noise",
            "Noise Suppression",
            ["Input noise floor", "Output noise floor", "Noise reduction", "SNR before", "SNR after", "SNR improvement"],
        )
        self._sii_labels = self._build_group(
            "sii",
            "Approximate Speech Intelligibility Index (SII)",
            ["SII before", "SII after", "SII improvement"],
        )
        self._residual_labels = self._build_group(
            "residual",
            "Residual (stage-to-stage, see README)",
            ["Beamform-stage residual energy", "Limiter-stage residual energy"],
        )
        self._steering_labels = self._build_group(
            "steering",
            "Steering",
            ["Commanded direction(s)", "Estimated direction", "Steering error"],
        )

        layout = QVBoxLayout(self)
        for box in (self._noise_group, self._sii_group, self._residual_group, self._steering_group):
            layout.addWidget(box)
        layout.addStretch(1)

    def _build_group(self, attr_prefix: str, title: str, field_names: list[str]) -> dict[str, QLabel]:
        group = QGroupBox(title)
        form = QFormLayout(group)
        labels: dict[str, QLabel] = {}
        for name in field_names:
            value_label = QLabel("—")
            form.addRow(QLabel(name + ":"), value_label)
            labels[name] = value_label
        setattr(self, f"_{attr_prefix}_group", group)
        return labels

    def clear(self) -> None:
        for labels in (self._noise_labels, self._sii_labels, self._residual_labels, self._steering_labels):
            for label in labels.values():
                label.setText("—")

    def update_metrics(self, metrics: dict) -> None:
        noise = metrics.get("noise_suppression", {})
        method = noise.get("method")
        self._noise_labels["Input noise floor"].setText(_metric_label(f"{noise.get('input_noise_floor_dbfs', 0):.1f} dBFS", method))
        self._noise_labels["Output noise floor"].setText(_metric_label(f"{noise.get('output_noise_floor_dbfs', 0):.1f} dBFS", method))
        self._noise_labels["Noise reduction"].setText(_metric_label(f"{noise.get('noise_reduction_db', 0):.1f} dB", method))
        self._noise_labels["SNR before"].setText(_metric_label(f"{noise.get('snr_before_db', 0):.1f} dB", method))
        self._noise_labels["SNR after"].setText(_metric_label(f"{noise.get('snr_after_db', 0):.1f} dB", method))
        self._noise_labels["SNR improvement"].setText(_metric_label(f"{noise.get('snr_improvement_db', 0):.1f} dB", method))

        sii = metrics.get("sii", {})
        sii_before = sii.get("sii_before", {})
        sii_after = sii.get("sii_after", {})
        sii_method = sii_before.get("method")
        self._sii_labels["SII before"].setText(_metric_label(f"{sii_before.get('value', 0):.3f}", sii_method))
        self._sii_labels["SII after"].setText(_metric_label(f"{sii_after.get('value', 0):.3f}", sii_after.get("method")))
        self._sii_labels["SII improvement"].setText(f"{sii.get('sii_improvement', 0):+.3f}")

        residual = metrics.get("residual", {})
        beamform_db = residual.get("beamform_stage_energy_ratio_db")
        limiter_db = residual.get("limiter_stage_energy_ratio_db")
        self._residual_labels["Beamform-stage residual energy"].setText(
            f"{beamform_db:.1f} dB" if beamform_db is not None else "—"
        )
        self._residual_labels["Limiter-stage residual energy"].setText(
            f"{limiter_db:.1f} dB" if limiter_db is not None else "—"
        )

        steering = metrics.get("steering", {})
        events = steering.get("commanded_events", [])
        commanded_text = ", ".join(f"{e['time_s']:.1f}s→{e['azimuth_deg']:.0f}°" for e in events) or "—"
        self._steering_labels["Commanded direction(s)"].setText(commanded_text)
        if steering.get("estimate_available"):
            self._steering_labels["Estimated direction"].setText("available")
            self._steering_labels["Steering error"].setText("see visualization")
        else:
            self._steering_labels["Estimated direction"].setText("not available (no DOA estimator in pipeline)")
            self._steering_labels["Steering error"].setText("not computable")
