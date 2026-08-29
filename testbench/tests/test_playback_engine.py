import numpy as np

from app.audio_io.playback_engine import apply_preamp, monitor_gain_linear, preamp_gain_linear


def test_monitor_gain_unity_at_zero_boost() -> None:
    assert monitor_gain_linear(1.0, 0.0) == 1.0
    assert monitor_gain_linear(0.5, 0.0) == 0.5


def test_monitor_gain_24db_is_about_16x() -> None:
    gain = monitor_gain_linear(1.0, 24.0)
    assert 15.0 < gain < 16.0


def test_preamp_gain_24db_is_about_16x() -> None:
    gain = preamp_gain_linear(24.0)
    assert 15.0 < gain < 16.0


def test_apply_preamp_scales_all_channels() -> None:
    block = np.full((8, 6), 0.01, dtype=np.float32)
    boosted = apply_preamp(block, 24.0)
    assert boosted.shape == block.shape
    assert np.allclose(boosted, 0.01 * preamp_gain_linear(24.0), rtol=0.01)


def test_apply_preamp_zero_db_is_unity() -> None:
    block = np.random.default_rng(0).normal(0, 0.05, (16, 6)).astype(np.float32)
    assert np.shares_memory(apply_preamp(block, 0.0), block)
