from __future__ import annotations

import numpy as np


def estimate_eta(z_true: np.ndarray, z_hat: np.ndarray, eps: float = 1e-8) -> float:
    """
    Eq. (14): eta = E[z_hat * z] / E[z_hat^2]
    Inputs can be shape [N] or [B, N].
    """
    z_true = np.asarray(z_true, dtype=np.float64)
    z_hat = np.asarray(z_hat, dtype=np.float64)
    if z_true.shape != z_hat.shape:
        raise ValueError("z_true and z_hat must have the same shape")
    num = np.mean(z_hat * z_true)
    den = np.mean(z_hat * z_hat) + eps
    return float(num / den)
