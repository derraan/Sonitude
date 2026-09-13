import numpy as np
import pytest

tf = pytest.importorskip("tensorflow")

from dsenet.model import DSENetConfig, build_streaming_step_model


def test_streaming_step_is_stateful():
    cfg = DSENetConfig(sample_rate_hz=16000, num_mics=3, hop_size=32, lookback=32, lookahead=32, hidden_size=32)
    model = build_streaming_step_model(cfg)
    rng = np.random.default_rng(0)
    pk = rng.standard_normal((1, cfg.feature_len), dtype=np.float32)
    zero = np.zeros((1, cfg.hidden_size), np.float32)
    hk_a, s1_a, s2_a = model([pk, zero, zero], training=False)
    hk_b, _, _ = model([pk, s1_a.numpy(), s2_a.numpy()], training=False)
    assert hk_a.shape[-1] == cfg.filters_flat_len
    assert not np.allclose(hk_a.numpy(), hk_b.numpy())
