import numpy as np

from dsenet.metrics import pair_metrics


def test_pair_metrics_improve_for_cleaner_estimate():
    rng = np.random.default_rng(7)
    target = rng.standard_normal(2000).astype(np.float32)
    noisy = target + 0.3 * rng.standard_normal(2000).astype(np.float32)
    cleaner = target + 0.1 * rng.standard_normal(2000).astype(np.float32)
    m_noisy = pair_metrics(target, noisy)
    m_clean = pair_metrics(target, cleaner)
    assert m_clean.snr_db > m_noisy.snr_db
    assert m_clean.si_sdr_db > m_noisy.si_sdr_db
