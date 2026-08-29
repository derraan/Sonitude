"""A graphical compass/dial widget for setting and displaying beam steering.

Drag anywhere on the dial (or click) to set the commanded azimuth, matching
the BeamformerSteering.azimuth_deg convention (0 deg = forward/front,
positive = right, per config/geometry_soundbubble_initial.yaml's zone
layout). An optional secondary needle can show an estimated direction when
one is available — the pipeline does not currently produce one (see
app/analysis/steering_error.py), so callers simply never set it and the
needle stays hidden.
"""

from __future__ import annotations

import math

from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent, QPen
from PySide6.QtWidgets import QWidget

_COMMANDED_COLOR = QColor("#2f7de1")
_ESTIMATED_COLOR = QColor("#e0a300")
_TICK_COLOR = QColor("#8a8a8a")
_LABEL_COLOR = QColor("#c8c8c8")


class SteeringDial(QWidget):
    azimuthChanged = Signal(float)  # emitted while dragging/clicking, degrees

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setMinimumSize(180, 180)
        self._commanded_azimuth_deg = 0.0
        self._estimated_azimuth_deg: float | None = None
        self._dragging = False

    def commanded_azimuth_deg(self) -> float:
        return self._commanded_azimuth_deg

    def set_commanded_azimuth_deg(self, azimuth_deg: float, *, emit: bool = False) -> None:
        self._commanded_azimuth_deg = self._normalize(azimuth_deg)
        self.update()
        if emit:
            self.azimuthChanged.emit(self._commanded_azimuth_deg)

    def set_estimated_azimuth_deg(self, azimuth_deg: float | None) -> None:
        """Set/clear the secondary "estimated direction" needle. None hides it."""
        self._estimated_azimuth_deg = None if azimuth_deg is None else self._normalize(azimuth_deg)
        self.update()

    @staticmethod
    def _normalize(azimuth_deg: float) -> float:
        wrapped = ((azimuth_deg + 180.0) % 360.0) - 180.0
        return wrapped

    def _geometry(self) -> tuple[QPointF, float]:
        side = min(self.width(), self.height())
        center = QPointF(self.width() / 2.0, self.height() / 2.0)
        radius = side / 2.0 - 16.0
        return center, radius

    def _azimuth_to_point(self, azimuth_deg: float, radius: float, center: QPointF) -> QPointF:
        # 0 deg = up (forward), positive azimuth = clockwise (to the right).
        theta = math.radians(azimuth_deg - 90.0)
        return QPointF(center.x() + radius * math.cos(theta), center.y() + radius * math.sin(theta))

    def _point_to_azimuth(self, point: QPointF, center: QPointF) -> float:
        dx = point.x() - center.x()
        dy = point.y() - center.y()
        theta_deg = math.degrees(math.atan2(dy, dx)) + 90.0
        return self._normalize(theta_deg)

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt override
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        center, radius = self._geometry()

        painter.setPen(QPen(_TICK_COLOR, 1.5))
        painter.drawEllipse(center, radius, radius)

        for label_deg in (0, 45, 90, 135, 180, -45, -90, -135):
            tick_outer = self._azimuth_to_point(label_deg, radius, center)
            tick_inner = self._azimuth_to_point(label_deg, radius - 8, center)
            painter.setPen(QPen(_TICK_COLOR, 1.5))
            painter.drawLine(tick_inner, tick_outer)
            label_point = self._azimuth_to_point(label_deg, radius + 12, center)
            painter.setPen(QPen(_LABEL_COLOR))
            painter.drawText(label_point, f"{label_deg}°" if label_deg != 0 else "0° (front)")

        if self._estimated_azimuth_deg is not None:
            tip = self._azimuth_to_point(self._estimated_azimuth_deg, radius * 0.75, center)
            painter.setPen(QPen(_ESTIMATED_COLOR, 3, Qt.PenStyle.DashLine))
            painter.drawLine(center, tip)

        tip = self._azimuth_to_point(self._commanded_azimuth_deg, radius * 0.85, center)
        painter.setPen(QPen(_COMMANDED_COLOR, 4))
        painter.drawLine(center, tip)
        painter.setBrush(_COMMANDED_COLOR)
        painter.drawEllipse(center, 5, 5)

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt override
        if event.button() == Qt.MouseButton.LeftButton:
            self._dragging = True
            self._update_from_mouse(event.position())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt override
        if self._dragging:
            self._update_from_mouse(event.position())

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt override
        del event
        self._dragging = False

    def _update_from_mouse(self, position: QPointF) -> None:
        center, _radius = self._geometry()
        azimuth = self._point_to_azimuth(position, center)
        self.set_commanded_azimuth_deg(azimuth, emit=True)
