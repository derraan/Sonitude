"""Secondary explanatory labels: inherit app font, use a muted palette role."""

from __future__ import annotations

from PySide6.QtGui import QPalette
from PySide6.QtWidgets import QLabel


def apply_secondary_note(label: QLabel) -> None:
    palette = label.palette()
    muted = palette.color(QPalette.ColorRole.PlaceholderText)
    if not muted.isValid() or muted.alpha() == 0:
        muted = palette.color(QPalette.ColorGroup.Disabled, QPalette.ColorRole.WindowText)
    palette.setColor(QPalette.ColorRole.WindowText, muted)
    label.setPalette(palette)
    label.setWordWrap(True)
