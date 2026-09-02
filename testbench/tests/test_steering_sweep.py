from __future__ import annotations

from app.analysis.steering_sweep import SteeringSweepResult, SweepPoint


def test_sweep_result_as_dict_round_trips_fields() -> None:
    result = SteeringSweepResult(
        expected_azimuth_deg=30.0,
        measured_peak_azimuth_deg=25.0,
        error_deg=-5.0,
        sweep=[SweepPoint(azimuth_deg=25.0, output_rms_dbfs=-12.3)],
    )
    as_dict = result.as_dict()
    assert as_dict["expected_azimuth_deg"] == 30.0
    assert as_dict["measured_peak_azimuth_deg"] == 25.0
    assert as_dict["error_deg"] == -5.0
    assert as_dict["sweep"] == [{"azimuth_deg": 25.0, "output_rms_dbfs": -12.3}]


def test_sweep_result_without_expected_has_no_error() -> None:
    result = SteeringSweepResult(
        expected_azimuth_deg=None,
        measured_peak_azimuth_deg=10.0,
        error_deg=None,
        sweep=[],
    )
    assert result.as_dict()["error_deg"] is None
