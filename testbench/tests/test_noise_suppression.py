from __future__ import annotations

import numpy as np

from app.analysis import noise_suppression


def test_rms_dbfs_of_silence_is_very_low() -> None:
    silence = np.zeros(1000, dtype=np.float32)
    assert noise_suppression.rms_dbfs(silence) < -100.0


def test_estimate_snr_improves_when_noise_reduced(mono_signal_pair, sample_rate: int) -> None:
    before, after = mono_signal_pair
    snr_before = noise_suppression.estimate_snr_db(before, sample_rate)
    snr_after = noise_suppression.estimate_snr_db(after, sample_rate)
    assert snr_after > snr_before


def test_compute_metrics_estimated_method(mono_signal_pair, sample_rate: int) -> None:
    before, after = mono_signal_pair
    metrics = noise_suppression.compute_noise_suppression_metrics(before, after, sample_rate)
    assert metrics.method == "estimated"
    assert metrics.snr_improvement_db > 0.0


def test_compute_metrics_reference_method(sample_rate: int) -> None:
    rng = np.random.default_rng(2)
    n = sample_rate
    noise_before = rng.normal(0, 0.15, n).astype(np.float32)
    noise_after = rng.normal(0, 0.02, n).astype(np.float32)
    tone = 0.3 * np.sin(2 * np.pi * 500 * np.linspace(0, 1, n, endpoint=False)).astype(np.float32)
    before = tone + noise_before
    after = tone + noise_after

    metrics = noise_suppression.compute_noise_suppression_metrics(
        before, after, sample_rate,
        noise_reference_before=noise_before, noise_reference_after=noise_after,
    )
    assert metrics.method == "reference"
    assert metrics.noise_reduction_db > 0.0
    assert metrics.snr_improvement_db > 0.0
    payload = metrics.as_dict()
    assert payload["definition"].startswith("mixture_power_over_noise_power")
    assert "mixture_to_noise_before_db" in payload
    assert payload["mixture_to_noise_improvement_db"] == payload["snr_improvement_db"]
