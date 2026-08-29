from app.ui.steering_controls import blend_percent_to_width_deg, width_deg_to_blend_percent


def test_blend_percent_maps_to_compat_width_deg() -> None:
    assert blend_percent_to_width_deg(0) == 0.0
    assert blend_percent_to_width_deg(100) == 180.0
    assert blend_percent_to_width_deg(50) == 90.0


def test_width_deg_round_trip_percent() -> None:
    for percent in (0, 25, 50, 75, 100):
        assert width_deg_to_blend_percent(blend_percent_to_width_deg(percent)) == percent
