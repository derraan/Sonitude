from __future__ import annotations

import tensorflow as tf


def si_sdr(target: tf.Tensor, estimate: tf.Tensor, eps: float = 1e-8) -> tf.Tensor:
    """
    Matches paper eq. (12)-(13).
    """
    target = tf.cast(target, tf.float32)
    estimate = tf.cast(estimate, tf.float32)
    alpha = tf.reduce_sum(target * estimate, axis=-1, keepdims=True) / (
        tf.reduce_sum(target * target, axis=-1, keepdims=True) + eps
    )
    target_proj = alpha * target
    noise = target_proj - estimate
    ratio = (tf.reduce_sum(target_proj * target_proj, axis=-1) + eps) / (
        tf.reduce_sum(noise * noise, axis=-1) + eps
    )
    return 10.0 * tf.math.log(ratio) / tf.math.log(tf.constant(10.0, dtype=tf.float32))


def neg_si_sdr_loss(target: tf.Tensor, estimate: tf.Tensor) -> tf.Tensor:
    return -tf.reduce_mean(si_sdr(target, estimate))
