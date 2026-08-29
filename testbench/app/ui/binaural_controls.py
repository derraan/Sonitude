"""Shared wiring for the binaural renderer widgets on both test-bench tabs.

Does not implement DSP. It only enables the controls the C++ tools advertise
and maps YAML / capability defaults onto the widgets.
"""

from __future__ import annotations

from PySide6.QtWidgets import QCheckBox, QComboBox, QDoubleSpinBox, QLabel

from app.config_reader import BinauralConfigSummary
from app.processing.capabilities import ToolCapabilities, preferred_binaural_backend


def sync_binaural_angle_widgets(
    *,
    follow: QCheckBox,
    azimuth: QDoubleSpinBox,
    elevation: QDoubleSpinBox,
    usable: bool,
) -> None:
    enabled = usable and not follow.isChecked()
    azimuth.setEnabled(enabled)
    elevation.setEnabled(enabled)


def populate_binaural_widgets(
    *,
    capabilities: ToolCapabilities,
    yaml_binaural: BinauralConfigSummary | None,
    enable: QCheckBox,
    backend: QComboBox,
    follow: QCheckBox,
    azimuth: QDoubleSpinBox,
    elevation: QDoubleSpinBox,
    note: QLabel,
    missing_query_message: str,
) -> None:
    caps = capabilities.binaural
    backend.clear()
    usable = bool(caps.available and capabilities.queried and caps.backends)
    enable.setEnabled(usable)
    backend.setEnabled(usable)
    follow.setEnabled(usable)

    if not capabilities.queried:
        enable.setChecked(False)
        sync_binaural_angle_widgets(follow=follow, azimuth=azimuth, elevation=elevation, usable=False)
        note.setText(missing_query_message)
        return
    if not usable:
        enable.setChecked(False)
        sync_binaural_angle_widgets(follow=follow, azimuth=azimuth, elevation=elevation, usable=False)
        note.setText("This C++ build reports binaural as unavailable.")
        return

    for name in caps.backends:
        backend.addItem(name, userData=name)

    yaml_backend = yaml_binaural.backend if yaml_binaural is not None else None
    chosen = preferred_binaural_backend(caps.backends, yaml_backend)
    if chosen is not None:
        index = backend.findData(chosen)
        if index >= 0:
            backend.setCurrentIndex(index)

    hrtf_ready = any(name != "mono_reference" for name in caps.backends)
    if yaml_binaural is not None:
        follow.setChecked(yaml_binaural.follow_steering)
        azimuth.setValue(yaml_binaural.azimuth_deg)
        elevation.setValue(yaml_binaural.elevation_deg)
        enable.setChecked(yaml_binaural.enabled or hrtf_ready)
    else:
        enable.setChecked(hrtf_ready)

    sync_binaural_angle_widgets(follow=follow, azimuth=azimuth, elevation=elevation, usable=True)
    unavailable = ", ".join(caps.unavailable_backends) or "none"
    binary = f" Binary: {capabilities.binary_path}." if capabilities.binary_path else ""
    note.setText(f"{caps.note} Unavailable backends (not offered): {unavailable}.{binary}")
