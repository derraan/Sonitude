"""Waveform / spectrogram / level-over-time visualization, via pyqtgraph.

pyqtgraph is used (rather than matplotlib) because it renders natively into
Qt widgets and stays responsive for the live updates real-time mode needs, as
well as for static batch-mode plots.
"""

from __future__ import annotations

import numpy as np
import pyqtgraph as pg
from PySide6.QtWidgets import QTabWidget, QVBoxLayout, QWidget
from scipy import signal as sps

_MAX_WAVEFORM_POINTS = 200_000
_RAW_COLOR = (120, 120, 120)
_PROCESSED_COLOR = (47, 125, 225)
_RESIDUAL_COLOR = (224, 90, 90)


def _downsample_for_plot(data: np.ndarray, max_points: int = _MAX_WAVEFORM_POINTS) -> tuple[np.ndarray, int]:
    if len(data) <= max_points:
        return data, 1
    stride = int(np.ceil(len(data) / max_points))
    return data[::stride], stride


class VisualizationPanel(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        pg.setConfigOptions(antialias=True)

        self.waveform_plot = pg.PlotWidget(title="Waveform")
        self.waveform_plot.addLegend()
        self.waveform_plot.setLabel("bottom", "Time", units="s")
        self.waveform_plot.setLabel("left", "Amplitude")

        self.spectrogram_view = pg.PlotWidget(title="Spectrogram")
        self.spectrogram_view.setLabel("bottom", "Time", units="s")
        self.spectrogram_view.setLabel("left", "Frequency", units="Hz")
        self._spectrogram_image = pg.ImageItem()
        self.spectrogram_view.addItem(self._spectrogram_image)
        self._spectrogram_colorbar = pg.ColorBarItem(colorMap="inferno")
        self._spectrogram_colorbar.setImageItem(self._spectrogram_image, insert_in=self.spectrogram_view.getPlotItem())

        self.levels_plot = pg.PlotWidget(title="RMS Level Over Time")
        self.levels_plot.addLegend()
        self.levels_plot.setLabel("bottom", "Time", units="s")
        self.levels_plot.setLabel("left", "Level", units="dBFS")

        tabs = QTabWidget()
        tabs.addTab(self.waveform_plot, "Waveform")
        tabs.addTab(self.spectrogram_view, "Spectrogram")
        tabs.addTab(self.levels_plot, "Levels")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(tabs)

    def plot_waveforms(
        self,
        sample_rate_hz: int,
        *,
        raw: np.ndarray | None = None,
        processed: np.ndarray | None = None,
        residual: np.ndarray | None = None,
    ) -> None:
        self.waveform_plot.clear()
        for data, color, name in (
            (raw, _RAW_COLOR, "raw"),
            (processed, _PROCESSED_COLOR, "processed"),
            (residual, _RESIDUAL_COLOR, "residual"),
        ):
            if data is None:
                continue
            mono = np.asarray(data).reshape(-1) if np.ndim(data) == 1 else np.asarray(data).mean(axis=1)
            plotted, stride = _downsample_for_plot(mono)
            time_axis = np.arange(len(plotted)) * stride / sample_rate_hz
            self.waveform_plot.plot(time_axis, plotted, pen=pg.mkPen(color=color, width=1), name=name)

    def plot_spectrogram(self, signal_data: np.ndarray, sample_rate_hz: int) -> None:
        mono = np.asarray(signal_data).reshape(-1) if np.ndim(signal_data) == 1 else np.asarray(signal_data).mean(axis=1)
        if len(mono) < 256:
            return
        freqs, times, sxx = sps.spectrogram(mono, fs=sample_rate_hz, nperseg=1024, noverlap=512)
        db = 10.0 * np.log10(sxx + 1e-12)
        self._spectrogram_image.setImage(db.T, autoLevels=True)
        if len(times) > 1 and len(freqs) > 1:
            self._spectrogram_image.setRect(0, 0, float(times[-1]), float(freqs[-1]))

    def plot_levels(self, signal_data: np.ndarray, sample_rate_hz: int, frame_ms: float = 20.0) -> None:
        mono = np.asarray(signal_data).reshape(-1) if np.ndim(signal_data) == 1 else np.asarray(signal_data).mean(axis=1)
        frame_len = max(1, int(sample_rate_hz * frame_ms / 1000.0))
        n_frames = max(1, len(mono) // frame_len)
        trimmed = mono[: n_frames * frame_len].reshape(n_frames, frame_len)
        rms = np.sqrt(np.mean(np.square(trimmed), axis=1) + 1e-12)
        db = 20.0 * np.log10(rms + 1e-12)
        time_axis = np.arange(n_frames) * frame_len / sample_rate_hz
        self.levels_plot.clear()
        self.levels_plot.plot(time_axis, db, pen=pg.mkPen(color=_PROCESSED_COLOR, width=1), name="level")

    def clear_all(self) -> None:
        self.waveform_plot.clear()
        self.levels_plot.clear()
        self._spectrogram_image.clear()
