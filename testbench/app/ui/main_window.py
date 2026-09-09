"""Top-level window: Sonitude Audio Algorithm Test Bench."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtWidgets import QMainWindow, QTabWidget

from app.config_reader import DEFAULT_CONFIG_PATH
from app.ui.calibration_tab import CalibrationTab
from app.ui.recorded_tab import RecordedDataTab
from app.ui.realtime_tab import RealtimeTab
from app.ui.upload_tab import UploadTab


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Sonitude Audio Algorithm Test Bench")
        self.resize(1280, 860)

        tabs = QTabWidget()
        self.recorded_tab = RecordedDataTab()
        self.realtime_tab = RealtimeTab()
        self.calibration_tab = CalibrationTab()
        tabs.addTab(self.calibration_tab, "Calibration")
        tabs.addTab(self.recorded_tab, "Recorded Data")
        tabs.addTab(self.realtime_tab, "Real-Time Audio")
        self.upload_tab = UploadTab(self.calibration_tab, self.recorded_tab, self.realtime_tab)
        tabs.addTab(self.upload_tab, "Upload")
        self.setCentralWidget(tabs)
        self.calibration_tab.dspConfigReady.connect(self._on_dsp_config_ready)
        self.upload_tab.runtimeConfigCommitted.connect(self._on_runtime_config_committed)

    def _on_dsp_config_ready(self, config_path: str) -> None:
        path = Path(config_path)
        self.recorded_tab.set_runtime_config_path(path)
        self.realtime_tab.set_runtime_config_path(path)

    def _on_runtime_config_committed(self, _config_path: str) -> None:
        self.recorded_tab.set_runtime_config_path(DEFAULT_CONFIG_PATH)
        self.realtime_tab.set_runtime_config_path(DEFAULT_CONFIG_PATH)

    def closeEvent(self, event) -> None:  # noqa: N802 - Qt override
        self.recorded_tab.save_layout()
        self.realtime_tab.save_layout()
        self.calibration_tab.save_layout()
        self.upload_tab.save_layout()
        preview = getattr(self.recorded_tab, "_preview", None)
        if preview is not None and preview.isRunning():
            preview.request_stop()
            preview.wait(3000)
        worker = self.realtime_tab._worker  # noqa: SLF001 - shutdown path only
        if worker is not None and worker.isRunning():
            worker.request_stop()
            worker.wait(3000)
        self.calibration_tab.shutdown()
        super().closeEvent(event)
