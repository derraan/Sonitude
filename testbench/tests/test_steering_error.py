from __future__ import annotations

from pathlib import Path

from app.analysis import steering_error
from app.processing.batch_adapter import write_steering_script
from app.storage.models import SteeringEvent


def test_write_and_parse_round_trip(tmp_path: Path) -> None:
    events = [
        SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0),
        SteeringEvent(time_s=2.5, azimuth_deg=45.0, elevation_deg=0.0, width_deg=30.0),
    ]
    path = write_steering_script(events, tmp_path / "script.csv")
    parsed = steering_error.parse_steering_script(path)
    assert len(parsed) == 2
    assert parsed[1].azimuth_deg == 45.0
    assert parsed[0].width_deg == 0.0
    assert parsed[1].width_deg == 30.0


def test_parse_legacy_three_column_script_defaults_width_to_zero(tmp_path: Path) -> None:
    path = tmp_path / "legacy.csv"
    path.write_text("# time_s,azimuth_deg,elevation_deg\n0.0,10.0,0.0\n", encoding="utf-8")
    parsed = steering_error.parse_steering_script(path)
    assert parsed[0].azimuth_deg == 10.0
    assert parsed[0].width_deg == 0.0


def test_commanded_direction_at_picks_last_event_before_time() -> None:
    events = [
        SteeringEvent(0.0, 0.0, 0.0),
        SteeringEvent(2.0, 45.0, 0.0),
        SteeringEvent(5.0, -30.0, 0.0),
    ]
    assert steering_error.commanded_direction_at(events, 1.0).azimuth_deg == 0.0
    assert steering_error.commanded_direction_at(events, 3.0).azimuth_deg == 45.0
    assert steering_error.commanded_direction_at(events, 10.0).azimuth_deg == -30.0


def test_compute_steering_error_without_estimate_marks_unavailable() -> None:
    events = [SteeringEvent(0.0, 0.0, 0.0)]
    samples = steering_error.compute_steering_error(events)
    assert len(samples) == 1
    assert samples[0].estimate_available is False
    assert samples[0].azimuth_error_deg is None


def test_compute_steering_error_with_estimate() -> None:
    commanded = [SteeringEvent(0.0, 30.0, 0.0)]
    estimated = [SteeringEvent(0.1, 28.0, 0.0)]
    samples = steering_error.compute_steering_error(commanded, estimated)
    assert samples[0].estimate_available is True
    assert samples[0].azimuth_error_deg == -2.0
