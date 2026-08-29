"""Unit tests for RealtimeWorker's active_channel_map handling — this is the
part that was previously buggy: live mode opened exactly 6 device channels
and used them in device order 0-5, ignoring config's active_channel_map
entirely, so any non-identity map (e.g. [2, 4, 6, 8, 10, 12]) silently
associated the wrong device channels with the array geometry/calibration.

These tests exercise the pure channel-selection logic without opening real
audio devices or a real subprocess.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf

from app.controller.realtime_controller import RealtimeWorker


def _make_worker(active_channel_map: list[int]) -> RealtimeWorker:
    # Constructing a RealtimeWorker doesn't open any device or subprocess —
    # those only happen inside run(), which these tests never call.
    return RealtimeWorker(
        input_device_index=0,
        config_path="unused.yaml",
        sample_rate_hz=44100,
        active_channel_map=active_channel_map,
    )


def test_identity_map_requires_exactly_six_device_channels() -> None:
    worker = _make_worker([0, 1, 2, 3, 4, 5])
    assert worker.device_channel_count() == 6


def test_non_identity_map_requires_enough_device_channels() -> None:
    worker = _make_worker([2, 4, 6, 8, 10, 12])
    assert worker.device_channel_count() == 13


def test_select_active_channels_reorders_and_subsets() -> None:
    worker = _make_worker([5, 3, 1, 0, 2, 4])
    # 6 frames, 6 device channels, each channel filled with its own index
    # so the selection/reorder is easy to verify by value.
    device_block = np.tile(np.arange(6, dtype=np.float32), (6, 1))
    selected = worker._select_active_channels(device_block)  # noqa: SLF001 - testing the core fix directly
    assert selected.shape == (6, 6)
    assert list(selected[0]) == [5.0, 3.0, 1.0, 0.0, 2.0, 4.0]


def test_select_active_channels_with_sparse_map_picks_correct_columns() -> None:
    worker = _make_worker([2, 4, 6, 8, 10, 12])
    device_block = np.tile(np.arange(13, dtype=np.float32), (4, 1))
    selected = worker._select_active_channels(device_block)  # noqa: SLF001
    assert selected.shape == (4, 6)
    assert list(selected[0]) == [2.0, 4.0, 6.0, 8.0, 10.0, 12.0]


def test_default_map_is_identity_when_none_given() -> None:
    worker = _make_worker([])
    assert worker.device_channel_count() == 6


def test_recording_streams_to_disk_without_ram_accumulation(tmp_path: Path) -> None:
    """Regression test for the bug this replaced: recordings used to
    accumulate unboundedly in Python lists in RAM. Now they stream straight
    to temp WAV files via a real sf.SoundFile handle, opened/closed only
    from _sync_recording_state (single-threaded call here, mirroring how
    run()'s loop calls it).
    """
    worker = _make_worker([0, 1, 2, 3, 4, 5])
    worker.set_recording(True)
    worker._sync_recording_state()  # noqa: SLF001
    assert worker._recording_active  # noqa: SLF001
    assert worker._raw_recording_file is not None  # noqa: SLF001

    raw_block = np.random.default_rng(1).normal(0, 0.1, (256, 6)).astype(np.float32)
    processed_block = np.random.default_rng(2).normal(0, 0.1, (256, 2)).astype(np.float32)
    worker._raw_recording_file.write(raw_block)  # noqa: SLF001
    worker._processed_recording_file.write(processed_block)  # noqa: SLF001

    worker.set_recording(False)
    worker._sync_recording_state()  # noqa: SLF001
    assert not worker._recording_active  # noqa: SLF001
    assert worker._raw_recording_file is None  # noqa: SLF001

    raw_out = tmp_path / "raw.wav"
    processed_out = tmp_path / "processed.wav"
    worker.save_raw_recording(raw_out)
    worker.save_processed_recording(processed_out)

    saved_raw, _ = sf.read(str(raw_out), dtype="float32")
    saved_processed, _ = sf.read(str(processed_out), dtype="float32")
    # Full float32 precision must survive (a prior version defaulted to
    # 16-bit PCM here, silently quantizing every recording).
    np.testing.assert_allclose(saved_raw, raw_block, atol=1e-6)
    np.testing.assert_allclose(saved_processed, processed_block, atol=1e-6)
