"""QSettings keys for test-bench splitter geometry. Ratios are starting points, not constants."""

from __future__ import annotations

from PySide6.QtCore import QByteArray, QSettings
from PySide6.QtWidgets import QSplitter

ORG = "Sonitude"
APP = "AudioAlgorithmTestBench"

KEY_RECORDED_H = "layout/recorded/horizontal"
KEY_RECORDED_V = "layout/recorded/vertical"
KEY_REALTIME_H = "layout/realtime/horizontal"
KEY_CALIBRATION_H = "layout/calibration/horizontal"
KEY_UPLOAD_H = "layout/upload/vertical"

# Initial recorded horizontal split: ~38% controls / 62% inspection at 1280 px.
RECORDED_H_DEFAULT = (486, 794)
RECORDED_V_DEFAULT = (420, 220)
REALTIME_H_DEFAULT = (460, 820)
CALIBRATION_H_DEFAULT = (540, 740)
UPLOAD_H_DEFAULT = (540, 220)


def settings() -> QSettings:
    return QSettings(ORG, APP)


def restore_splitter(splitter: QSplitter, key: str, default_sizes: tuple[int, ...]) -> None:
    stored = settings().value(key)
    data: QByteArray | None = None
    if isinstance(stored, QByteArray):
        data = stored
    elif isinstance(stored, (bytes, bytearray)):
        data = QByteArray(stored)
    if data is not None and not data.isEmpty() and splitter.restoreState(data):
        return
    splitter.setSizes(list(default_sizes))


def save_splitter(splitter: QSplitter, key: str) -> None:
    settings().setValue(key, splitter.saveState())
