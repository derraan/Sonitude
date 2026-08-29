from app.audio_io.playback_engine import monitor_gain_linear


def test_monitor_gain_unity_at_zero_boost() -> None:
    assert monitor_gain_linear(1.0, 0.0) == 1.0
    assert monitor_gain_linear(0.5, 0.0) == 0.5


def test_monitor_gain_24db_is_about_16x() -> None:
    gain = monitor_gain_linear(1.0, 24.0)
    assert 15.0 < gain < 16.0
