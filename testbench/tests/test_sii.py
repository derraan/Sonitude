from __future__ import annotations

from app.analysis import sii


def test_sii_value_in_unit_range(mono_signal_pair, sample_rate: int) -> None:
    before, _ = mono_signal_pair
    result = sii.compute_sii(before, sample_rate)
    assert 0.0 <= result.value <= 1.0
    assert result.method == "estimated"
    assert len(result.band_snr_db) == len(sii._BAND_CENTERS_HZ)


def test_sii_improves_for_cleaner_signal(mono_signal_pair, sample_rate: int) -> None:
    before, after = mono_signal_pair
    comparison = sii.compute_sii_before_after(before, after, sample_rate)
    assert comparison.improvement > 0.0
    assert comparison.sii_after.value >= comparison.sii_before.value


def test_importance_weights_sum_to_one() -> None:
    assert abs(sii._IMPORTANCE.sum() - 1.0) < 1e-9
