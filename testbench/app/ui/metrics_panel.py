"""Displays the metrics.json produced for a test result: noise suppression,
the experimental intelligibility proxy, residual energy, and steering. Every
value that is an estimate rather than a precise measurement is labeled as
such, per testbench/README.md's metric definitions.
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
            ["Input noise floor", "Output noise floor", "Noise reduction", "Mixture-to-noise before", "Mixture-to-noise after", "Mixture-to-noise improvement"],
        )
        self._sii_labels = self._build_group(
            "sii",
            "Experimental Intelligibility Proxy (not a standardized SII)",
            ["Proxy before", "Proxy after", "Proxy improvement"],
        )
        self._residual_labels = self._build_group(
            "residual",
            "Residual (stage-to-stage, see README)",
            ["Beamform-stage residual energy", "Limiter-stage residual energy"],
        )
        self._steering_labels = self._build_group(
            "steering",
            "Steering",
            ["Commanded direction(s)", "Expected direction", "Measured peak (sweep)", "Steering error"],
        )

        layout = QVBoxLayout(self)
        for box in (self._noise_group, self._sii_group, self._residual_group, self._steering_group):
            layout.addWidget(box)
        layout.addStretch(1)

    def _build_group(self, attr_prefix: str, title: str, field_names: list[str]) -> dict[str, QLabel]:
        group = QGroupBox(title)
        form = QFormLayout(group)
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        labels: dict[str, QLabel] = {}
        for name in field_names:
            value_label = QLabel("—")
            value_label.setWordWrap(True)
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
        self._noise_labels["Mixture-to-noise before"].setText(
            _metric_label(f"{noise.get('mixture_to_noise_before_db', noise.get('snr_before_db', 0)):.1f} dB", method)
        )
        self._noise_labels["Mixture-to-noise after"].setText(
            _metric_label(f"{noise.get('mixture_to_noise_after_db', noise.get('snr_after_db', 0)):.1f} dB", method)
        )
        self._noise_labels["Mixture-to-noise improvement"].setText(
            _metric_label(
                f"{noise.get('mixture_to_noise_improvement_db', noise.get('snr_improvement_db', 0)):.1f} dB",
                method,
            )
        )

        proxy = metrics.get("intelligibility_proxy", {})
        proxy_before = proxy.get("sii_before", {})
        proxy_after = proxy.get("sii_after", {})
        self._sii_labels["Proxy before"].setText(_metric_label(f"{proxy_before.get('value', 0):.3f}", proxy_before.get("method")))
        self._sii_labels["Proxy after"].setText(_metric_label(f"{proxy_after.get('value', 0):.3f}", proxy_after.get("method")))
        self._sii_labels["Proxy improvement"].setText(f"{proxy.get('sii_improvement', 0):+.3f}")

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
        commanded_text = ", ".join(
            f"{e['time_s']:.1f}s→{e['azimuth_deg']:.0f}° (blend {e.get('directivity_blend_deg', e.get('width_deg', 0)):.0f}°)" for e in events
        ) or "—"
        self._steering_labels["Commanded direction(s)"].setText(commanded_text)

        sweep = steering.get("objective_sweep_test")
        if sweep:
            self._steering_labels["Expected direction"].setText(
                f"{sweep['expected_azimuth_deg']:.0f}°" if sweep.get("expected_azimuth_deg") is not None else "—"
            )
            self._steering_labels["Measured peak (sweep)"].setText(f"{sweep['measured_peak_azimuth_deg']:.0f}°")
            error = sweep.get("error_deg")
            self._steering_labels["Steering error"].setText(f"{error:+.1f}°" if error is not None else "—")
        else:
            self._steering_labels["Expected direction"].setText("not set (steering sweep not run)")
            self._steering_labels["Measured peak (sweep)"].setText("not available")
            self._steering_labels["Steering error"].setText("not computable — enable the steering sweep test")
