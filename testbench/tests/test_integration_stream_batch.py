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
