"""A graphical compass/dial widget for setting and displaying beam steering.

Drag, click, arrow keys, or an adjacent spin box (see SteeringControls) set
the commanded azimuth, matching BeamformerSteering.azimuth_deg (0 deg =
forward/front, positive = right). An optional secondary needle can show an
estimated direction when one is available.
"""

from __future__ import annotations

import math

from PySide6.QtCore import QPointF, QSize, Qt, Signal
from PySide6.QtGui import QBrush, QColor, QKeyEvent, QMouseEvent, QPainter, QPaintEvent, QPen
from PySide6.QtWidgets import QSizePolicy, QWidget

_COMMANDED_COLOR = QColor("#2f7de1")
_ESTIMATED_COLOR = QColor("#e0a300")
_TICK_COLOR = QColor("#8a8a8a")
_LABEL_COLOR = QColor("#c8c8c8")
_WIDTH_WEDGE_COLOR = QColor(47, 125, 225, 60)
_MAX_WIDTH_DEG = 180.0
_MIN_SIDE = 120
_PREFERRED_SIDE = 180


class SteeringDial(QWidget):
    azimuthChanged = Signal(float)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setMinimumSize(_MIN_SIDE, _MIN_SIDE)
        self.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Preferred)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setAccessibleName("Commanded steering azimuth")
        self.setAccessibleDescription(
            "Circular steering control. Zero degrees is forward. Positive azimuth is to the right. "
            "Use arrow keys, the azimuth spin box, or click and drag."
        )
        self._commanded_azimuth_deg = 0.0
        self._estimated_azimuth_deg: float | None = None
        self._width_deg = 0.0
        self._dragging = False

    def sizeHint(self) -> QSize:  # noqa: N802
        return QSize(_PREFERRED_SIDE, _PREFERRED_SIDE)

    def minimumSizeHint(self) -> QSize:  # noqa: N802
        return QSize(_MIN_SIDE, _MIN_SIDE)

    def commanded_azimuth_deg(self) -> float:
        return self._commanded_azimuth_deg

    def width_deg(self) -> float:
        return self._width_deg

    def set_width_deg(self, width_deg: float) -> None:
        """Show the directional/omni blend as a shaded wedge around the
        commanded needle. This is a mix toward the six-microphone average,
        not a measured physical beamwidth / HPBW."""
        self._width_deg = max(0.0, min(_MAX_WIDTH_DEG, width_deg))
        self.update()

    def set_commanded_azimuth_deg(self, azimuth_deg: float, *, emit: bool = False) -> None:
        self._commanded_azimuth_deg = self._normalize(azimuth_deg)
        self.setAccessibleDescription(
            f"Commanded steering azimuth {self._commanded_azimuth_deg:.0f} degrees. "
            "Zero is forward. Positive is to the right. Range minus 180 to plus 180."
        )
        self.update()
        if emit:
            self.azimuthChanged.emit(self._commanded_azimuth_deg)

    def set_estimated_azimuth_deg(self, azimuth_deg: float | None) -> None:
        self._estimated_azimuth_deg = None if azimuth_deg is None else self._normalize(azimuth_deg)
        self.update()

    @staticmethod
    def _normalize(azimuth_deg: float) -> float:
        return ((azimuth_deg + 180.0) % 360.0) - 180.0

    def _geometry(self) -> tuple[QPointF, float]:
        side = min(self.width(), self.height())
        center = QPointF(self.width() / 2.0, self.height() / 2.0)
        radius = side / 2.0 - 16.0
        return center, radius

    def _azimuth_to_point(self, azimuth_deg: float, radius: float, center: QPointF) -> QPointF:
        theta = math.radians(azimuth_deg - 90.0)
        return QPointF(center.x() + radius * math.cos(theta), center.y() + radius * math.sin(theta))

    def _point_to_azimuth(self, point: QPointF, center: QPointF) -> float:
        dx = point.x() - center.x()
        dy = point.y() - center.y()
        theta_deg = math.degrees(math.atan2(dy, dx)) + 90.0
        return self._normalize(theta_deg)

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        center, radius = self._geometry()

        if self.hasFocus():
            painter.setPen(QPen(_COMMANDED_COLOR, 2, Qt.PenStyle.DashLine))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRect(self.rect().adjusted(2, 2, -2, -2))

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

        if self._width_deg > 0.0:
            wedge_radius = radius * 0.85
            top_left = QPointF(center.x() - wedge_radius, center.y() - wedge_radius)
            rect = (top_left.x(), top_left.y(), wedge_radius * 2, wedge_radius * 2)
            start_qt_angle = int((90.0 - (self._commanded_azimuth_deg + self._width_deg / 2.0)) * 16)
            span_qt_angle = int(self._width_deg * 16)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QBrush(_WIDTH_WEDGE_COLOR))
            painter.drawPie(*rect, start_qt_angle, span_qt_angle)

        if self._estimated_azimuth_deg is not None:
            tip = self._azimuth_to_point(self._estimated_azimuth_deg, radius * 0.75, center)
            painter.setPen(QPen(_ESTIMATED_COLOR, 3, Qt.PenStyle.DashLine))
            painter.drawLine(center, tip)

        tip = self._azimuth_to_point(self._commanded_azimuth_deg, radius * 0.85, center)
        painter.setPen(QPen(_COMMANDED_COLOR, 4))
        painter.drawLine(center, tip)
        painter.setBrush(_COMMANDED_COLOR)
        painter.drawEllipse(center, 5, 5)

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802
        step = 15.0 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 1.0
        if event.key() in (Qt.Key.Key_Left, Qt.Key.Key_Down):
            self.set_commanded_azimuth_deg(self._commanded_azimuth_deg - step, emit=True)
            event.accept()
            return
        if event.key() in (Qt.Key.Key_Right, Qt.Key.Key_Up):
            self.set_commanded_azimuth_deg(self._commanded_azimuth_deg + step, emit=True)
            event.accept()
            return
        super().keyPressEvent(event)

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self.setFocus(Qt.FocusReason.MouseFocusReason)
            self._dragging = True
            self._update_from_mouse(event.position())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if self._dragging:
            self._update_from_mouse(event.position())

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        del event
        self._dragging = False

    def _update_from_mouse(self, position: QPointF) -> None:
        center, _radius = self._geometry()
        azimuth = self._point_to_azimuth(position, center)
        self.set_commanded_azimuth_deg(azimuth, emit=True)
