"""Top-level window: Sonitude Audio Algorithm Test Bench."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtWidgets import QMainWindow, QTabWidget

from app.config_reader import DEFAULT_CONFIG_PATH
from app.controller.runtime_yaml_catalog import calibration_path_from_runtime
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
        cal = calibration_path_from_runtime(path)
        self.recorded_tab.apply_pipeline_files(path, cal, restart_if_running=True)
        self.realtime_tab.apply_pipeline_files(path, cal, restart_if_running=True)

    def _on_runtime_config_committed(self, config_path: str) -> None:
        path = Path(config_path) if config_path else DEFAULT_CONFIG_PATH
        cal = calibration_path_from_runtime(path)
        self.recorded_tab.apply_pipeline_files(path, cal, restart_if_running=True)
        self.realtime_tab.apply_pipeline_files(path, cal, restart_if_running=True)

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
