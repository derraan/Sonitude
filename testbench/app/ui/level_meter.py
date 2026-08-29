"""Horizontal dBFS meter: bar plus current/peak numeric readout."""

from __future__ import annotations

import numpy as np
from PySide6.QtWidgets import QHBoxLayout, QLabel, QProgressBar, QWidget

_LEVEL_METER_MIN_DBFS = -60.0
_CLIP_DBFS = -0.1


def dbfs_to_percent(dbfs: float) -> int:
    return int(np.clip((dbfs - _LEVEL_METER_MIN_DBFS) / -_LEVEL_METER_MIN_DBFS * 100.0, 0, 100))


class DbfsMeter(QWidget):
    def __init__(self, name: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._peak_dbfs = _LEVEL_METER_MIN_DBFS
        self._name = QLabel(name)
        self._name.setMinimumWidth(28)
        self._bar = QProgressBar()
        self._bar.setRange(0, 100)
        self._bar.setTextVisible(False)
        self._bar.setAccessibleName(f"{name} level")
        self._value = QLabel("— dBFS")
        self._value.setMinimumWidth(120)

        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(self._name)
        row.addWidget(self._bar, stretch=1)
        row.addWidget(self._value)

    def reset_peak(self) -> None:
        self._peak_dbfs = _LEVEL_METER_MIN_DBFS
        self._value.setText("— dBFS")
        self._bar.setValue(0)
        self._bar.setStyleSheet("")

    def set_dbfs(self, dbfs: float) -> None:
        self._peak_dbfs = max(self._peak_dbfs, dbfs)
        clipped = dbfs >= _CLIP_DBFS
        self._bar.setValue(dbfs_to_percent(dbfs))
        self._value.setText(f"{dbfs:.1f} / pk {self._peak_dbfs:.1f} dBFS")
        self._bar.setStyleSheet("QProgressBar::chunk { background-color: #c0392b; }" if clipped else "")
        if clipped:
            self._value.setText(f"{dbfs:.1f} / pk {self._peak_dbfs:.1f} dBFS CLIP")
