from __future__ import annotations

import numpy as np
import pytest

from app.analysis import residual


def test_identical_signals_have_zero_residual() -> None:
    signal = np.ones(100, dtype=np.float32)
    result = residual.compute_stage_residual(signal, signal)
    assert np.allclose(result, 0.0)


def test_residual_is_pre_minus_post() -> None:
    pre = np.array([1.0, 2.0, 3.0])
    post = np.array([0.5, 1.0, 1.5])
    result = residual.compute_stage_residual(pre, post)
    assert np.allclose(result, [0.5, 1.0, 1.5])


def test_rejects_mismatched_channel_counts() -> None:
    mono = np.zeros(10)
    stereo = np.zeros((10, 2))
    with pytest.raises(residual.IncompatibleSignalsError):
        residual.compute_stage_residual(mono, stereo)


def test_trim_to_shortest_handles_length_mismatch() -> None:
    a = np.arange(10)
    b = np.arange(7)
    a2, b2 = residual.trim_to_shortest(a, b)
    assert len(a2) == len(b2) == 7


def test_residual_energy_ratio_db_zero_when_unchanged() -> None:
    signal = np.random.default_rng(0).normal(0, 0.1, 1000)
    db = residual.residual_energy_ratio_db(signal, signal)
    assert db == float("-inf")


def test_residual_energy_ratio_db_negative_when_suppressed() -> None:
    rng = np.random.default_rng(1)
    pre = rng.normal(0, 0.2, 2000)
    post = pre * 0.1  # suppressed heavily
    db = residual.residual_energy_ratio_db(pre, post)
    assert db < 0.0
