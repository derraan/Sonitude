"""End-to-end streaming batch against sonitude_stream_process."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf

from app.audio_io import audio_loader
from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.processing.batch_adapter import run_stream_batch
from app.storage.models import SteeringEvent


def test_stream_batch_writes_playable_stereo(
    tmp_path: Path, stream_process_binary: Path
) -> None:
    config_summary = read_runtime_config_summary(DEFAULT_CONFIG_PATH)
    sample_rate = config_summary.capture_sample_rate_hz
    n = int(sample_rate * 0.25)
    t = np.linspace(0, 0.25, n, endpoint=False)
    tone = 0.2 * np.sin(2 * np.pi * 440 * t)
    data = np.stack([tone * (0.4 + 0.05 * ch) for ch in range(6)], axis=1).astype(np.float32)
    source = tmp_path / "six.wav"
    sf.write(str(source), data, sample_rate, subtype="FLOAT")

    result = run_stream_batch(
        source,
        DEFAULT_CONFIG_PATH,
        [SteeringEvent(0.0, 0.0, 0.0, 0.0)],
        tmp_path / "out",
        binary_path=stream_process_binary,
        active_channel_map=config_summary.active_channel_map,
        block_frames=1024,
    )
    assert result.streaming is True
    assert result.frames == n
    stereo = tmp_path / "out" / "processed_stereo.wav"
    preview = tmp_path / "out" / "raw_preview_stereo.wav"
    assert stereo.exists()
    assert preview.exists()
    pcm, sr = audio_loader.load_wav(stereo)
    assert sr == sample_rate
    assert pcm.shape[1] == 2
    assert len(pcm) == n
    assert np.any(np.abs(pcm) > 1e-6)
    info = sf.info(str(stereo))
    assert info.subtype == "PCM_16"


def test_streaming_batch_honours_flac_export(
    tmp_path: Path, stream_process_binary: Path, monkeypatch
) -> None:
    from PySide6.QtCore import QCoreApplication

    from app.audio_io.exporter import read_back
    from app.controller.batch_controller import BatchWorker
    from app.storage.result_store import ResultStore

    if QCoreApplication.instance() is None:
        QCoreApplication([])

    monkeypatch.setattr("app.controller.batch_controller.should_stream_batch", lambda _meta: True)

    config_summary = read_runtime_config_summary(DEFAULT_CONFIG_PATH)
    sample_rate = config_summary.capture_sample_rate_hz
    n = int(sample_rate * 0.25)
    t = np.linspace(0, 0.25, n, endpoint=False)
    tone = 0.2 * np.sin(2 * np.pi * 440 * t)
    data = np.stack([tone * (0.4 + 0.05 * ch) for ch in range(6)], axis=1).astype(np.float32)
    source = tmp_path / "six.wav"
    sf.write(str(source), data, sample_rate, subtype="FLOAT")

    store = ResultStore(tmp_path)
    worker = BatchWorker(
        [source],
        DEFAULT_CONFIG_PATH,
        [SteeringEvent(0.0, 0.0, 0.0, 0.0)],
        output_container="flac",
        result_store=store,
    )
    result = worker._process_one(source)
    export = store.results_dir / result.test_id / "processed_export.flac"
    stereo = store.results_dir / result.test_id / "processed_stereo.wav"
    assert export.exists()
    info = sf.info(str(export))
    stereo_info = sf.info(str(stereo))
    assert info.format == "FLAC"
    assert info.frames == stereo_info.frames
    assert info.channels == stereo_info.channels
    assert info.samplerate == stereo_info.samplerate
    decoded, sr = read_back(export)
    wav_decoded, _ = read_back(stereo)
    assert sr == sample_rate
    quantized = np.round(wav_decoded * (2**23)) / (2**23)
    np.testing.assert_allclose(decoded, quantized, atol=2 / (2**23))
