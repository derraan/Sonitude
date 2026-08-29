"""Batch (Mode 1) processing controller: runs on a background QThread so the
GUI stays responsive while sonitude_wav_replay and the Python-side metrics
run for each file.

This is the only place batch-mode pieces (adapter, analysis, storage) are
wired together; the GUI never calls them directly.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

import soundfile as sf
from PySide6.QtCore import QThread, Signal

from app.analysis import noise_suppression, residual, sii, steering_error, steering_sweep
from app.audio_io import downmix, wav_loader
from app.config_reader import read_runtime_config_summary
from app.processing.batch_adapter import BatchProcessingError, run_wav_replay
from app.storage.models import SteeringEvent
from app.storage.result_store import ResultStore
from app.version_info import read_algorithm_version

logger = logging.getLogger(__name__)


@dataclass
class FileResult:
    input_path: str
    test_id: str
    metrics: dict


class BatchWorker(QThread):
    progress = Signal(int, int)  # (files_completed, files_total)
    file_started = Signal(str)
    file_finished = Signal(str, dict)  # input_path, {"test_id": ..., "metrics": {...}}
    file_failed = Signal(str, str)  # input_path, error message
    finished_all = Signal()

    def __init__(
        self,
        input_paths: list[str | Path],
        config_path: str | Path,
        steering_events: list[SteeringEvent],
        *,
        enable_suppression: bool = False,
        disable_limiter: bool = False,
        steering_test_expected_azimuth_deg: float | None = None,
        result_store: ResultStore | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self._input_paths = [Path(p) for p in input_paths]
        self._config_path = Path(config_path)
        self._steering_events = steering_events
        self._enable_suppression = enable_suppression
        self._disable_limiter = disable_limiter
        # When set, an objective steering sweep (see analysis/steering_sweep.py)
        # runs against each file: the SAME algorithm is re-rendered at a range
        # of candidate azimuths and the measured energy-peak direction is
        # compared to this expected/ground-truth angle for the test fixture.
        # Optional because it costs several extra full-file renders per file.
        self._steering_test_expected_azimuth_deg = steering_test_expected_azimuth_deg
        self._result_store = result_store or ResultStore()
        self._stop_requested = False

    def request_stop(self) -> None:
        self._stop_requested = True

    def run(self) -> None:  # noqa: D102 - QThread entrypoint
        total = len(self._input_paths)
        for index, input_path in enumerate(self._input_paths):
            if self._stop_requested:
                break
            self.file_started.emit(str(input_path))
            try:
                result = self._process_one(input_path)
                self.file_finished.emit(str(input_path), {"test_id": result.test_id, "metrics": result.metrics})
            except Exception as exc:  # noqa: BLE001 - reported to the GUI, not crashed on
                logger.exception("Batch processing failed for %s", input_path)
                self.file_failed.emit(str(input_path), str(exc))
            self.progress.emit(index + 1, total)
        self.finished_all.emit()

    def _process_one(self, input_path: Path) -> FileResult:
        config_summary = read_runtime_config_summary(self._config_path)
        validation = wav_loader.validate_six_channel_wav(
            input_path, expected_sample_rate_hz=config_summary.capture_sample_rate_hz
        )
        if not validation.ok:
            raise ValueError("; ".join(validation.errors))

        test = self._result_store.new_test()
        self._result_store.copy_input(input_path, test.input_wav)

        try:
            batch_result = run_wav_replay(
                input_wav=input_path,
                config_path=self._config_path,
                steering_events=self._steering_events,
                output_dir=test.root,
                enable_suppression=self._enable_suppression,
                disable_limiter=self._disable_limiter,
            )
        except BatchProcessingError as exc:
            raise RuntimeError(str(exc)) from exc

        beamformed, sample_rate = wav_loader.load_wav(batch_result.beamformed_wav)
        suppressed, _ = wav_loader.load_wav(batch_result.suppressed_wav)
        processed, _ = wav_loader.load_wav(batch_result.output_wav)
        beamformed_mono = beamformed[:, 0]
        suppressed_mono = suppressed[:, 0]
        processed_mono = processed[:, 0]

        # Stereo playback copies (algorithm output is mono; duplicate to L/R,
        # same convention as main.cpp's mono-to-stereo passthrough).
        sf.write(str(test.processed_stereo_wav), downmix.mono_to_stereo(processed_mono), sample_rate)

        # RAW listening preview: ear-cup mic pair from the original input.
        raw_data, raw_sample_rate = wav_loader.load_wav(input_path)
        raw_six = wav_loader.extract_channels(raw_data, config_summary.active_channel_map)
        sf.write(
            str(test.raw_preview_wav),
            downmix.ear_cup_stereo_preview(raw_six, config_summary.active_channel_map),
            raw_sample_rate,
        )

        # Residuals: same-domain, same-alignment stage-to-stage subtraction only.
        beamform_residual = residual.compute_stage_residual(beamformed_mono, suppressed_mono)
        limiter_residual = residual.compute_stage_residual(suppressed_mono, processed_mono)
        sf.write(str(test.residual_beamform_wav), downmix.mono_to_stereo(beamform_residual), sample_rate)
        sf.write(str(test.residual_limiter_wav), downmix.mono_to_stereo(limiter_residual), sample_rate)

        noise_metrics = noise_suppression.compute_noise_suppression_metrics(
            beamformed_mono, suppressed_mono, sample_rate
        )
        sii_result = sii.compute_sii_before_after(beamformed_mono, processed_mono, sample_rate)
        commanded_events = steering_error.parse_steering_script(test.steering_script)

        sweep_result = None
        estimated_events = None
        if self._steering_test_expected_azimuth_deg is not None:
            sweep_result = steering_sweep.run_steering_sweep(
                input_path,
                self._config_path,
                expected_azimuth_deg=self._steering_test_expected_azimuth_deg,
            )
            estimated_events = [
                SteeringEvent(time_s=0.0, azimuth_deg=sweep_result.measured_peak_azimuth_deg, elevation_deg=0.0)
            ]
        steering_samples = steering_error.compute_steering_error(commanded_events, estimated_events)

        steering_metrics = {
            "commanded_events": [
                {"time_s": e.time_s, "azimuth_deg": e.azimuth_deg, "elevation_deg": e.elevation_deg, "width_deg": e.width_deg}
                for e in commanded_events
            ],
            "estimate_available": estimated_events is not None,
        }
        if sweep_result is not None:
            steering_metrics["objective_sweep_test"] = sweep_result.as_dict()
            steering_metrics["error_samples"] = [
                {
                    "time_s": s.time_s,
                    "commanded_azimuth_deg": s.commanded_azimuth_deg,
                    "estimated_azimuth_deg": s.estimated_azimuth_deg,
                    "azimuth_error_deg": s.azimuth_error_deg,
                }
                for s in steering_samples
            ]
            steering_metrics["note"] = (
                "estimate_available reflects an energy-peak beam-response sweep against the "
                "expected direction below, not an independent DOA estimator — see "
                "analysis/steering_sweep.py."
            )
        else:
            steering_metrics["note"] = (
                "No DOA estimator exists in the pipeline, and no steering sweep test was run for "
                "this file; only commanded direction is available."
            )

        metrics = {
            "noise_suppression": noise_metrics.as_dict(),
            "intelligibility_proxy": sii_result.as_dict(),
            "residual": {
                "beamform_stage_energy_ratio_db": residual.residual_energy_ratio_db(
                    beamformed_mono, suppressed_mono
                ),
                "limiter_stage_energy_ratio_db": residual.residual_energy_ratio_db(
                    suppressed_mono, processed_mono
                ),
            },
            "steering": steering_metrics,
        }

        metadata = {
            "test_id": test.test_id,
            "input_file": str(input_path),
            "sample_rate_hz": sample_rate,
            "channels": 6,
            "duration_s": len(beamformed_mono) / sample_rate if sample_rate else 0.0,
            "algorithm_version": read_algorithm_version(),
            "processing_parameters": {
                "config_path": str(self._config_path),
                "enable_suppression": self._enable_suppression,
                "disable_limiter": self._disable_limiter,
            },
            "created_at": datetime.now(timezone.utc).isoformat(),
            "metrics": metrics,
        }

        self._result_store.save_metadata(test, metadata)
        self._result_store.save_metrics(test, metrics)
        return FileResult(input_path=str(input_path), test_id=test.test_id, metrics=metrics)
