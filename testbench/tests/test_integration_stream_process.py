"""End-to-end integration test against the REAL sonitude_stream_process
binary. Skipped when the binary hasn't been built — see
test_integration_wav_replay.py's module docstring for the rationale.
"""

from __future__ import annotations

import numpy as np
import pytest

from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.processing.sonitude_binary_locator import MissingBinaryError, find_binary
from app.processing.stream_adapter import StreamProcessor

try:
    _STREAM_PROCESS_BINARY = find_binary("sonitude_stream_process")
except MissingBinaryError:
    _STREAM_PROCESS_BINARY = None

pytestmark = pytest.mark.skipif(
    _STREAM_PROCESS_BINARY is None,
    reason="sonitude_stream_process not built; see testbench/README.md build instructions",
)


def test_stream_process_round_trips_blocks() -> None:
    sample_rate = read_runtime_config_summary(DEFAULT_CONFIG_PATH).capture_sample_rate_hz
    processor = StreamProcessor(DEFAULT_CONFIG_PATH, sample_rate_hz=sample_rate, binary_path=_STREAM_PROCESS_BINARY)
    try:
        rng = np.random.default_rng(3)
        block = rng.normal(0, 0.05, (256, 6)).astype(np.float32)
        processor.send_block(block, azimuth_deg=0.0, elevation_deg=0.0, suppression_focus_active=True)
        processed = processor.recv_block()
        assert processed.shape == (256, 2)
        assert np.all(np.isfinite(processed))
    finally:
        processor.close()


def test_stream_process_width_changes_output() -> None:
    sample_rate = read_runtime_config_summary(DEFAULT_CONFIG_PATH).capture_sample_rate_hz
    processor = StreamProcessor(DEFAULT_CONFIG_PATH, sample_rate_hz=sample_rate, binary_path=_STREAM_PROCESS_BINARY)
    try:
        rng = np.random.default_rng(4)
        block = rng.normal(0, 0.05, (256, 6)).astype(np.float32)
        processor.send_block(block, 0.0, 0.0, True, width_deg=0.0)
        narrow = processor.recv_block()
        processor.send_block(block, 0.0, 0.0, True, width_deg=180.0)
        wide = processor.recv_block()
        assert not np.allclose(narrow, wide, atol=1e-6)
    finally:
        processor.close()
