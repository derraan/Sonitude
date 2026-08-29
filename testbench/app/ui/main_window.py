"""Top-level window: Sonitude Audio Algorithm Test Bench."""

from __future__ import annotations

from PySide6.QtWidgets import QMainWindow, QTabWidget

from app.ui.recorded_tab import RecordedDataTab
from app.ui.realtime_tab import RealtimeTab


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Sonitude Audio Algorithm Test Bench")
        self.resize(1280, 860)

        tabs = QTabWidget()
        self.recorded_tab = RecordedDataTab()
        self.realtime_tab = RealtimeTab()
        tabs.addTab(self.recorded_tab, "Recorded Data")
        tabs.addTab(self.realtime_tab, "Real-Time Audio")
        self.setCentralWidget(tabs)

    def closeEvent(self, event) -> None:  # noqa: N802 - Qt override
        self.recorded_tab.save_layout()
        self.realtime_tab.save_layout()
        worker = self.realtime_tab._worker  # noqa: SLF001 - shutdown path only
        if worker is not None and worker.isRunning():
            worker.request_stop()
            worker.wait(3000)
        super().closeEvent(event)
