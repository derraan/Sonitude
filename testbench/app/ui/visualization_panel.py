"""Waveform / spectrogram / level-over-time visualization, via pyqtgraph."""

from __future__ import annotations

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QLabel, QTabWidget, QVBoxLayout, QWidget
from scipy import signal as sps

from app.ui.secondary_note import apply_secondary_note

_MAX_WAVEFORM_POINTS = 8_000
_RAW_COLOR = (120, 120, 120)
_PROCESSED_COLOR = (47, 125, 225)
_RESIDUAL_COLOR = (224, 90, 90)


def _downsample_for_plot(data: np.ndarray, max_points: int = _MAX_WAVEFORM_POINTS) -> tuple[np.ndarray, int]:
    if len(data) <= max_points:
        return data, 1
    stride = int(np.ceil(len(data) / max_points))
    return data[::stride], stride


class VisualizationPanel(QWidget):
    def __init__(
        self,
        parent: QWidget | None = None,
        *,
        show_spectrogram: bool = True,
        show_levels: bool = True,
    ) -> None:
        super().__init__(parent)
        pg.setConfigOptions(antialias=False)
        self.setMinimumHeight(160)

        self.waveform_plot = pg.PlotWidget(title="Waveform")
        self.waveform_plot.addLegend()
        self.waveform_plot.setLabel("bottom", "Time", units="s")
        self.waveform_plot.setLabel("left", "Amplitude")

        tabs = QTabWidget()
        tabs.addTab(self.waveform_plot, "Waveform")

        self.spectrogram_view: pg.PlotWidget | None = None
        self.levels_plot: pg.PlotWidget | None = None
        self._spectrogram_image: pg.ImageItem | None = None

        if show_spectrogram:
            self.spectrogram_view = pg.PlotWidget(title="Spectrogram")
            self.spectrogram_view.setLabel("bottom", "Time", units="s")
            self.spectrogram_view.setLabel("left", "Frequency", units="Hz")
            self._spectrogram_image = pg.ImageItem()
            self.spectrogram_view.addItem(self._spectrogram_image)
            colorbar = pg.ColorBarItem(colorMap="inferno")
            colorbar.setImageItem(self._spectrogram_image, insert_in=self.spectrogram_view.getPlotItem())
            tabs.addTab(self.spectrogram_view, "Spectrogram")

        if show_levels:
            self.levels_plot = pg.PlotWidget(title="RMS Level Over Time")
            self.levels_plot.addLegend()
            self.levels_plot.setLabel("bottom", "Time", units="s")
            self.levels_plot.setLabel("left", "Level", units="dBFS")
            tabs.addTab(self.levels_plot, "Levels")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(tabs)
        if not show_spectrogram or not show_levels:
            note = QLabel("Live mode shows waveform only. Spectrogram and RMS-over-time are recorded-mode plots.")
            apply_secondary_note(note)
            layout.addWidget(note)

    def plot_waveforms(
        self,
        sample_rate_hz: int,
        *,
        raw: np.ndarray | None = None,
        processed: np.ndarray | None = None,
        residual: np.ndarray | None = None,
        emphasize: str | None = None,
        duration_s: float | None = None,
    ) -> None:
        self.waveform_plot.clear()
        styles = {
            "raw": Qt.PenStyle.SolidLine,
            "processed": Qt.PenStyle.DashLine,
            "residual": Qt.PenStyle.DotLine,
        }
        for data, color, name in (
            (raw, _RAW_COLOR, "raw"),
            (processed, _PROCESSED_COLOR, "processed"),
            (residual, _RESIDUAL_COLOR, "residual"),
        ):
            if data is None:
                continue
            mono = np.asarray(data).reshape(-1) if np.ndim(data) == 1 else np.asarray(data).mean(axis=1)
            plotted, stride = _downsample_for_plot(mono)
            if duration_s is not None and duration_s > 0 and len(plotted) > 1:
                time_axis = np.linspace(0.0, duration_s, len(plotted), dtype=np.float64)
            else:
                time_axis = np.arange(len(plotted)) * stride / sample_rate_hz
            width = 2.4 if emphasize == name else 1.4
            self.waveform_plot.plot(
                time_axis,
                plotted,
                pen=pg.mkPen(color=color, width=width, style=styles[name]),
                name=name,
            )

    def plot_spectrogram(
        self,
        signal_data: np.ndarray,
        sample_rate_hz: int,
        *,
        duration_s: float | None = None,
        freqs: np.ndarray | None = None,
        spectrogram_db: np.ndarray | None = None,
    ) -> None:
        if self._spectrogram_image is None:
            return
        if spectrogram_db is not None and freqs is not None and duration_s:
            db = np.asarray(spectrogram_db)
            self._spectrogram_image.setImage(db.T, autoLevels=True)
            nyquist = float(freqs[-1]) if len(freqs) else sample_rate_hz / 2.0
            self._spectrogram_image.setRect(0, 0, float(duration_s), nyquist)
            return
        mono = np.asarray(signal_data).reshape(-1) if np.ndim(signal_data) == 1 else np.asarray(signal_data).mean(axis=1)
        if len(mono) < 256:
            return
        nperseg = min(256, max(32, len(mono) // 8 * 2 or 32))
        nperseg = min(nperseg, len(mono))
        freqs_hz, times, sxx = sps.spectrogram(mono, fs=sample_rate_hz, nperseg=nperseg, noverlap=nperseg // 2)
        db = 10.0 * np.log10(sxx + 1e-12)
        self._spectrogram_image.setImage(db.T, autoLevels=True)
        width = float(duration_s) if duration_s and duration_s > 0 else float(times[-1] if len(times) else 0.0)
        height = float(freqs_hz[-1] if len(freqs_hz) else sample_rate_hz / 2.0)
        if width > 0 and height > 0:
            self._spectrogram_image.setRect(0, 0, width, height)

    def plot_levels(
        self,
        signal_data: np.ndarray,
        sample_rate_hz: int,
        frame_ms: float = 20.0,
        *,
        duration_s: float | None = None,
        rms_dbfs: np.ndarray | None = None,
    ) -> None:
        if self.levels_plot is None:
            return
        if rms_dbfs is not None and duration_s and len(rms_dbfs) > 1:
            db = np.asarray(rms_dbfs, dtype=np.float64)
            time_axis = np.linspace(0.0, duration_s, len(db), dtype=np.float64)
        else:
            mono = np.asarray(signal_data).reshape(-1) if np.ndim(signal_data) == 1 else np.asarray(signal_data).mean(axis=1)
            frame_len = max(1, int(sample_rate_hz * frame_ms / 1000.0))
            n_frames = max(1, len(mono) // frame_len)
            trimmed = mono[: n_frames * frame_len].reshape(n_frames, frame_len)
            rms = np.sqrt(np.mean(np.square(trimmed), axis=1) + 1e-12)
            db = 20.0 * np.log10(rms + 1e-12)
            if duration_s and duration_s > 0:
                time_axis = np.linspace(0.0, duration_s, n_frames, dtype=np.float64)
            else:
                time_axis = np.arange(n_frames) * frame_len / sample_rate_hz
        self.levels_plot.clear()
        self.levels_plot.plot(
            time_axis,
            db,
            pen=pg.mkPen(color=_PROCESSED_COLOR, width=1.4, style=Qt.PenStyle.DashLine),
            name="level",
        )

    def clear_all(self) -> None:
        self.waveform_plot.clear()
        if self.levels_plot is not None:
            self.levels_plot.clear()
        if self._spectrogram_image is not None:
            self._spectrogram_image.clear()
