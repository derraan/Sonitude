import numpy as np

from dsenet.fas_reference import (
    filter_and_sum_with_interpolation,
    interpolate_filters,
    naive_filter_and_sum,
)


def test_interpolated_fas_matches_naive():
    rng = np.random.default_rng(42)
    num_mics = 6
    hop = 32
    filter_len = 65
    frame = rng.standard_normal((num_mics, hop + filter_len - 1)).astype(np.float32)
    h_prev = rng.standard_normal((num_mics, filter_len)).astype(np.float32)
    h_cur = rng.standard_normal((num_mics, filter_len)).astype(np.float32)

    out1 = filter_and_sum_with_interpolation(frame, h_prev, h_cur, hop)
    out2 = naive_filter_and_sum(frame, interpolate_filters(h_prev, h_cur, hop))
    np.testing.assert_allclose(out1, out2, rtol=1e-5, atol=5e-6)
