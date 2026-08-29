"""End-to-end integration test against the REAL sonitude_stream_process binary.

When SONITUDE_REQUIRE_CPP=1 (CI), a missing binary is a failure, not a skip.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.processing.protocol import PROTOCOL_VERSION
from app.processing.stream_adapter import StreamProcessor


def test_stream_process_round_trips_blocks(stream_process_binary: Path) -> None:
    sample_rate = read_runtime_config_summary(DEFAULT_CONFIG_PATH).capture_sample_rate_hz
    processor = StreamProcessor(
        DEFAULT_CONFIG_PATH, sample_rate_hz=sample_rate, binary_path=stream_process_binary
    )
    try:
        assert processor.protocol_version == PROTOCOL_VERSION
        assert processor._process.poll() is None  # noqa: SLF001 - prove the real process is running
        rng = np.random.default_rng(3)
        block = rng.normal(0, 0.05, (256, 6)).astype(np.float32)
        seq = processor.send_block(block, azimuth_deg=0.0, elevation_deg=0.0, suppression_focus_active=True)
        processed, echoed = processor.recv_block()
        assert echoed == seq
        assert processed.shape == (256, 2)
        assert np.all(np.isfinite(processed))
        exe_name = Path(stream_process_binary).name
        assert "sonitude_stream_process" in exe_name
    finally:
        processor.close()
        assert processor._process.poll() is not None  # noqa: SLF001


def test_stream_process_blend_changes_output(stream_process_binary: Path) -> None:
    sample_rate = read_runtime_config_summary(DEFAULT_CONFIG_PATH).capture_sample_rate_hz
    processor = StreamProcessor(
        DEFAULT_CONFIG_PATH, sample_rate_hz=sample_rate, binary_path=stream_process_binary
    )
    try:
        rng = np.random.default_rng(4)
        block = rng.normal(0, 0.05, (256, 6)).astype(np.float32)
        processor.send_block(block, 0.0, 0.0, True, width_deg=0.0)
        narrow, seq0 = processor.recv_block()
        processor.send_block(block, 0.0, 0.0, True, width_deg=180.0)
        wide, seq1 = processor.recv_block()
        assert seq1 == seq0 + 1
        assert not np.allclose(narrow, wide, atol=1e-6)
    finally:
        processor.close()


def test_stream_process_wrong_sequence_is_detected_locally() -> None:
    from app.processing.protocol import ProtocolError, assert_response_sequence

    with pytest.raises(ProtocolError, match="sequence"):
        assert_response_sequence(actual=0, expected=1)
