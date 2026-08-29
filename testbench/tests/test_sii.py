from __future__ import annotations

import numpy as np

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


def test_noise_only_negative_control_is_not_an_acceptance_metric(sample_rate: int) -> None:
    """Noise compared with itself must not be treated as a plausible speech SII.

    A standardized SII for noise-only should not look like mid-range speech
    intelligibility. This experimental proxy can still produce an intermediate
    score; that is why it is labeled experimental and must not gate acceptance.
    """
    rng = np.random.default_rng(99)
    noise = rng.normal(0, 0.1, sample_rate).astype(np.float32)
    result = sii.compute_sii(noise, sample_rate, noise_reference=noise)
    payload = result.as_dict()
    assert payload["experimental"] is True
    assert payload["standardized"] is False
    assert payload["acceptance_gating"] is False
    assert payload["standard"] == "NOT ANSI/ASA S3.5 SII"
    # Nearby wrong property: treating ~0.5 as "speech is somewhat intelligible".
    # The proxy is allowed to be non-zero; it is not allowed to claim S3.5.
    assert 0.0 <= result.value <= 1.0

