"""Batch (Mode 1) processing controller: runs on a background QThread so the
GUI stays responsive while sonitude_wav_replay and the Python-side metrics
run for each file.

This is the only place batch-mode pieces (adapter, analysis, storage) are
wired together; the GUI never calls them directly. Production DSP stays in C++.
"""

from __future__ import annotations

import json
import logging
import shutil
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from PySide6.QtCore import QThread, Signal

from app.analysis import noise_suppression, residual, sii, steering_error, steering_sweep
from app.audio_io import audio_loader, downmix
from app.audio_io.exporter import export_pcm, transcode_lossless
from app.audio_io.stream_io import COPY_BYTES_LIMIT, should_stream_batch
from app.config_reader import read_runtime_config_summary
from app.processing.batch_adapter import BatchProcessingError, run_stream_batch, run_wav_replay
from app.processing.protocol import PROTOCOL_VERSION
from app.processing.suppression import SuppressionMode, parse_suppression_mode, resolve_suppression
from app.storage.models import BinauralRequest, SteeringEvent
from app.storage.result_store import ResultStore
from app.version_info import read_algorithm_version, read_git_commit

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
    file_progress = Signal(int)  # 0-100 within the current file
    finished_all = Signal()

    def __init__(
        self,
        input_paths: list[str | Path],
        config_path: str | Path,
        steering_events: list[SteeringEvent],
        *,
        suppression: SuppressionMode | str = SuppressionMode.AUTO,
        enable_suppression: bool | None = None,
        suppression_backend: str | None = None,
        disable_limiter: bool = False,
        output_container: str = "wav",
        binaural: BinauralRequest | None = None,
        steering_test_expected_azimuth_deg: float | None = None,
        result_store: ResultStore | None = None,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self._input_paths = [Path(p) for p in input_paths]
        self._config_path = Path(config_path)
        self._steering_events = steering_events
        mode = parse_suppression_mode(suppression)
        if enable_suppression is True:
            mode = SuppressionMode.ON
        elif enable_suppression is False:
            mode = SuppressionMode.OFF
        self._suppression = mode
        self._suppression_backend = suppression_backend
        self._disable_limiter = disable_limiter
        self._output_container = output_container.lower().lstrip(".")
        self._binaural = binaural or BinauralRequest()
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
        suppression_state = resolve_suppression(self._suppression, config_summary.suppression_enabled)
        validation = audio_loader.validate_audio_file(
            input_path,
            expected_sample_rate_hz=config_summary.capture_sample_rate_hz,
            active_channel_map=config_summary.active_channel_map,
        )
        if not validation.ok:
            kind = validation.error_kind or "dsp_input"
            raise ValueError(f"{kind}: " + "; ".join(validation.errors))

        test = self._result_store.new_test()
        original_copy = test.root / f"input_original{input_path.suffix.lower()}"
        if input_path.stat().st_size <= COPY_BYTES_LIMIT:
            self._result_store.copy_input(input_path, original_copy)
            source_for_decode = original_copy
        else:
            (test.root / "input_original.ref.txt").write_text(str(input_path.resolve()), encoding="utf-8")
            source_for_decode = input_path
        if test.runtime_config_copy is not None:
            shutil.copy2(self._config_path, test.runtime_config_copy)

        metadata = validation.metadata
        use_stream = metadata is not None and should_stream_batch(metadata)
        if use_stream:
            return self._process_one_streaming(
                input_path,
                test,
                source_for_decode,
                original_copy,
                config_summary,
                suppression_state,
                validation,
            )

        try:
            batch_result = run_wav_replay(
                input_wav=source_for_decode,
                config_path=self._config_path,
                steering_events=self._steering_events,
                output_dir=test.root,
                suppression=self._suppression,
                suppression_backend=self._suppression_backend,
                disable_limiter=self._disable_limiter,
                binaural=self._binaural,
                expected_sample_rate_hz=config_summary.capture_sample_rate_hz,
                active_channel_map=config_summary.active_channel_map,
            )
        except BatchProcessingError as exc:
            raise RuntimeError(str(exc)) from exc

        if batch_result.decoded_input_wav != test.input_wav:
            shutil.copy2(batch_result.decoded_input_wav, test.input_wav)

        beamformed, sample_rate = audio_loader.load_wav(batch_result.beamformed_wav)
        suppressed, _ = audio_loader.load_wav(batch_result.suppressed_wav)
        processed, _ = audio_loader.load_wav(batch_result.output_wav)
        beamformed_mono = beamformed[:, 0]
        suppressed_mono = suppressed[:, 0]
        processed_mono = processed[:, 0]

        export_pcm(
            test.processed_stereo_wav,
            downmix.mono_to_stereo(processed_mono),
            sample_rate,
            container="wav",
            subtype="PCM_16",
        )
        export_suffix = "flac" if self._output_container == "flac" else "wav"
        export_path = test.root / f"processed_export.{export_suffix}"
        export_pcm(export_path, processed_mono, sample_rate, container=export_suffix)
        test.processed_export = export_path

        raw_data, raw_meta = audio_loader.load_audio(source_for_decode)
        raw_six = audio_loader.extract_channels(raw_data, config_summary.active_channel_map)
        export_pcm(
            test.raw_preview_wav,
            downmix.ear_cup_stereo_preview(raw_six, config_summary.active_channel_map),
            raw_meta.sample_rate_hz,
            container="wav",
            subtype="PCM_16",
        )

        cpp_resolved = batch_result.resolved or {}
        suppression_delay = int(float(cpp_resolved.get("suppression_algorithmic_delay_samples") or 0))
        beamformed_aligned, suppressed_aligned = residual.delay_align(
            beamformed_mono, suppressed_mono, suppression_delay
        )
        processed_for_sii = processed_mono
        if suppression_delay > 0 and suppression_delay < len(processed_mono):
            _, processed_for_sii = residual.delay_align(
                beamformed_mono, processed_mono, suppression_delay
            )

        # Residuals: same-domain DSP taps only. Never use the ear-cup preview.
        # Spectral STFT delay is removed from the beamformed reference first.
        beamform_residual = residual.compute_stage_residual(
            beamformed_mono, suppressed_mono, delay_samples=suppression_delay
        )
        limiter_residual = residual.compute_stage_residual(suppressed_mono, processed_mono)
        export_pcm(test.residual_beamform_wav, downmix.mono_to_stereo(beamform_residual), sample_rate, container="wav", subtype="PCM_16")
        export_pcm(test.residual_limiter_wav, downmix.mono_to_stereo(limiter_residual), sample_rate, container="wav", subtype="PCM_16")

        if batch_result.binaural_wav is not None:
            test.binaural_wav = batch_result.binaural_wav

        noise_metrics = noise_suppression.compute_noise_suppression_metrics(
            beamformed_aligned, suppressed_aligned, sample_rate
        )
        proxy_result = sii.compute_sii_before_after(beamformed_aligned, processed_for_sii, sample_rate)
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
                {
                    "time_s": e.time_s,
                    "azimuth_deg": e.azimuth_deg,
                    "elevation_deg": e.elevation_deg,
                    "directivity_blend_deg": e.directivity_blend_deg,
                    "width_deg": e.width_deg,
                }
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
            "intelligibility_proxy": {
                **proxy_result.as_dict(),
                "acceptance_gating": False,
                "experimental": True,
                "standardized": False,
            },
            "residual": {
                "beamform_stage_energy_ratio_db": residual.residual_energy_ratio_db(
                    beamformed_mono, suppressed_mono, delay_samples=suppression_delay
                ),
                "limiter_stage_energy_ratio_db": residual.residual_energy_ratio_db(
                    suppressed_mono, processed_mono
                ),
                "listening_preview_excluded": True,
                "alignment_delay_samples": suppression_delay,
            },
            "steering": steering_metrics,
            "suppression_backend_requested": self._suppression_backend,
            "suppression_backend_resolved": cpp_resolved.get("suppression_backend_resolved"),
            "suppression_resolved": cpp_resolved.get("suppression_resolved"),
        }

        input_meta = validation.metadata.as_dict() if validation.metadata is not None else None
        if test.input_metadata_json is not None and input_meta is not None:
            test.input_metadata_json.write_text(json.dumps(input_meta, indent=2), encoding="utf-8")

        metadata = {
            "test_id": test.test_id,
            "input_file": str(input_path),
            "input": {
                "path": str(input_path),
                "name": input_path.name,
                "format": input_meta["container"] if input_meta else None,
                "sample_rate_hz": input_meta["sample_rate_hz"] if input_meta else None,
                "channel_count": input_meta["channels"] if input_meta else None,
                "subtype": input_meta["subtype"] if input_meta else None,
                "original_copy": str(original_copy),
            },
            "active_channel_map": config_summary.active_channel_map,
            "config_path": str(self._config_path),
            "geometry_path": config_summary.geometry_path or None,
            "calibration_path": config_summary.calibration_path or None,
            "steering": steering_metrics["commanded_events"],
            "suppression": {
                "requested": suppression_state.requested.value,
                "backend_requested": self._suppression_backend,
                "backend_resolved": cpp_resolved.get("suppression_backend_resolved"),
                "yaml_enabled": suppression_state.yaml_enabled,
                "resolved_enabled": (
                    cpp_resolved.get("suppression_resolved")
                    if "suppression_resolved" in cpp_resolved
                    else suppression_state.resolved_enabled
                ),
                "algorithmic_delay_samples": cpp_resolved.get("suppression_algorithmic_delay_samples"),
                "implementation_status": cpp_resolved.get("suppression_implementation_status") or None,
                "cpp_resolved": cpp_resolved or None,
            },
            "binaural": {
                "requested": self._binaural.as_dict(),
                "resolved": {
                    "backend": cpp_resolved.get("binaural_backend"),
                    "available": cpp_resolved.get("binaural_available"),
                }
                if cpp_resolved
                else None,
            },
            "limiter_disabled": self._disable_limiter,
            "output_format": export_suffix,
            "protocol_version": None,
            "stream_protocol_version": None,
            "capabilities_protocol_version": PROTOCOL_VERSION,
            "sonitude_version": read_algorithm_version() or None,
            "git_commit": read_git_commit(),
            "sample_rate_hz": sample_rate,
            "channels": 6,
            "duration_s": len(beamformed_mono) / sample_rate if sample_rate else 0.0,
            "algorithm_version": read_algorithm_version(),
            "processing_parameters": {
                "config_path": str(self._config_path),
                "suppression_requested": self._suppression.value,
                "disable_limiter": self._disable_limiter,
                "output_container": export_suffix,
            },
            "created_at": datetime.now(timezone.utc).isoformat(),
            "metrics": metrics,
            "cpp_command": batch_result.command,
        }

        self._result_store.save_metadata(test, metadata)
        self._result_store.save_metrics(test, metrics)
        return FileResult(input_path=str(input_path), test_id=test.test_id, metrics=metrics)

    def _process_one_streaming(
        self,
        input_path,
        test,
        source_for_decode,
        original_copy,
        config_summary,
        suppression_state,
        validation,
    ) -> FileResult:
        def _progress(done: int, total: int) -> None:
            self.file_progress.emit(min(100, int(done * 100 / total)) if total else 0)

        try:
            batch_result = run_stream_batch(
                input_wav=source_for_decode,
                config_path=self._config_path,
                steering_events=self._steering_events,
                output_dir=test.root,
                suppression=self._suppression,
                suppression_backend=self._suppression_backend,
                disable_limiter=self._disable_limiter,
                binaural=self._binaural,
                active_channel_map=config_summary.active_channel_map,
                progress_callback=_progress,
            )
        except BatchProcessingError as exc:
            raise RuntimeError(str(exc)) from exc

        sample_rate = batch_result.sample_rate_hz or (
            validation.metadata.sample_rate_hz if validation.metadata is not None else 0
        )
        export_suffix = "flac" if self._output_container == "flac" else "wav"
        export_path = test.root / f"processed_export.{export_suffix}"
        if export_suffix == "flac":
            transcode_lossless(test.processed_stereo_wav, export_path, container="flac")
            test.processed_export = export_path
        else:
            test.processed_export = test.processed_stereo_wav
        if batch_result.binaural_wav is not None:
            test.binaural_wav = batch_result.binaural_wav

        noise_metrics = noise_suppression.compute_noise_suppression_metrics_from_wavs(
            str(test.raw_preview_wav), str(test.processed_stereo_wav), sample_rate
        )
        commanded_events = steering_error.parse_steering_script(test.steering_script)
        steering_metrics = {
            "commanded_events": [
                {
                    "time_s": e.time_s,
                    "azimuth_deg": e.azimuth_deg,
                    "elevation_deg": e.elevation_deg,
                    "directivity_blend_deg": e.directivity_blend_deg,
                    "width_deg": e.width_deg,
                }
                for e in commanded_events
            ],
            "estimate_available": False,
            "note": (
                "Streaming batch used sonitude_stream_process so the file is never fully "
                "loaded. Objective steering sweep and DSP-tap residuals are skipped."
            ),
        }
        skipped_proxy = {
            "method": "skipped_streaming_batch",
            "value": 0.0,
            "band_snr_db": [],
            "experimental": True,
            "standardized": False,
            "acceptance_gating": False,
            "standard": "NOT ANSI/ASA S3.5 SII",
        }
        cpp_resolved = batch_result.resolved or {}
        metrics = {
            "noise_suppression": noise_metrics.as_dict(),
            "intelligibility_proxy": {
                "sii_before": skipped_proxy,
                "sii_after": skipped_proxy,
                "sii_improvement": 0.0,
                "acceptance_gating": False,
                "experimental": True,
                "standardized": False,
                "note": "Full-file intelligibility proxy is skipped for streaming batch.",
            },
            "residual": {
                "beamform_stage_energy_ratio_db": None,
                "limiter_stage_energy_ratio_db": None,
                "listening_preview_excluded": True,
                "note": "wav_replay taps are not available on the streaming path.",
            },
            "steering": steering_metrics,
            "pipeline": "stream_process",
            "suppression_backend_requested": self._suppression_backend,
            "suppression_backend_resolved": cpp_resolved.get("suppression_backend_resolved"),
            "suppression_resolved": cpp_resolved.get("suppression_resolved"),
        }
        input_meta = validation.metadata.as_dict() if validation.metadata is not None else None
        if test.input_metadata_json is not None and input_meta is not None:
            test.input_metadata_json.write_text(json.dumps(input_meta, indent=2), encoding="utf-8")
        duration_s = (
            batch_result.frames / sample_rate
            if sample_rate
            else (validation.metadata.duration_s if validation.metadata is not None else 0.0)
        )
        metadata = {
            "test_id": test.test_id,
            "input_file": str(input_path),
            "input": {
                "path": str(input_path),
                "name": input_path.name,
                "format": input_meta["container"] if input_meta else None,
                "sample_rate_hz": input_meta["sample_rate_hz"] if input_meta else None,
                "channel_count": input_meta["channels"] if input_meta else None,
                "subtype": input_meta["subtype"] if input_meta else None,
                "original_copy": str(original_copy if original_copy.exists() else source_for_decode),
            },
            "active_channel_map": config_summary.active_channel_map,
            "config_path": str(self._config_path),
            "geometry_path": config_summary.geometry_path or None,
            "calibration_path": config_summary.calibration_path or None,
            "steering": steering_metrics["commanded_events"],
            "suppression": {
                "requested": suppression_state.requested.value,
                "backend_requested": self._suppression_backend,
                "backend_resolved": cpp_resolved.get("suppression_backend_resolved"),
                "yaml_enabled": suppression_state.yaml_enabled,
                "resolved_enabled": (
                    cpp_resolved.get("suppression_resolved")
                    if "suppression_resolved" in cpp_resolved
                    else suppression_state.resolved_enabled
                ),
                "algorithmic_delay_samples": cpp_resolved.get("suppression_algorithmic_delay_samples"),
                "implementation_status": cpp_resolved.get("suppression_implementation_status") or None,
                "cpp_resolved": cpp_resolved or None,
            },
            "binaural": {
                "requested": self._binaural.as_dict(),
                "resolved": {
                    "backend": cpp_resolved.get("binaural_backend"),
                    "available": cpp_resolved.get("binaural_available"),
                }
                if cpp_resolved
                else None,
            },
            "limiter_disabled": self._disable_limiter,
            "output_format": export_suffix,
            "stream_protocol_version": PROTOCOL_VERSION,
            "capabilities_protocol_version": PROTOCOL_VERSION,
            "sonitude_version": read_algorithm_version() or None,
            "git_commit": read_git_commit(),
            "sample_rate_hz": sample_rate,
            "channels": 6,
            "duration_s": duration_s,
            "algorithm_version": read_algorithm_version(),
            "processing_parameters": {
                "config_path": str(self._config_path),
                "suppression_requested": self._suppression.value,
                "disable_limiter": self._disable_limiter,
                "pipeline": "stream_process",
            },
            "created_at": datetime.now(timezone.utc).isoformat(),
            "metrics": metrics,
            "cpp_command": batch_result.command,
        }
        self._result_store.save_metadata(test, metadata)
        self._result_store.save_metrics(test, metrics)
        self.file_progress.emit(100)
        return FileResult(input_path=str(input_path), test_id=test.test_id, metrics=metrics)
