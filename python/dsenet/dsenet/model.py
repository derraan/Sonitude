from __future__ import annotations

from dataclasses import dataclass

import tensorflow as tf


@dataclass(frozen=True)
class DSENetConfig:
    sample_rate_hz: int = 16000
    num_mics: int = 3
    hop_size: int = 32
    lookback: int = 32
    lookahead: int = 32
    hidden_size: int = 128
    l2_eps: float = 1e-8
    layer_norm_eps: float = 1e-6

    @property
    def filter_len(self) -> int:
        return 1 + self.lookback + self.lookahead

    @property
    def feature_len(self) -> int:
        return self.num_mics * (self.hop_size + self.lookback + self.lookahead)

    @property
    def filters_flat_len(self) -> int:
        return self.num_mics * self.filter_len


def _l2_normalize(x: tf.Tensor, eps: float) -> tf.Tensor:
    norm = tf.sqrt(tf.reduce_sum(tf.square(x), axis=-1, keepdims=True) + eps)
    return x / norm


def build_dsenet_filter_estimator(config: DSENetConfig) -> tf.keras.Model:
    """
    Builds paper Section II-C neural stack:
      p_k -> L2 normalize -> FC+PReLU -> LN -> GRU -> GRU -> FC(h_k)
    """
    pk_in = tf.keras.Input(shape=(config.feature_len,), name="p_k")
    x = tf.keras.layers.Lambda(lambda t: _l2_normalize(t, config.l2_eps), name="l2norm")(pk_in)
    x = tf.keras.layers.Dense(config.hidden_size, name="feature_fc")(x)
    x = tf.keras.layers.PReLU(shared_axes=None, name="feature_prelu")(x)
    x = tf.keras.layers.LayerNormalization(epsilon=config.layer_norm_eps, name="feature_ln")(x)

    x_seq = tf.keras.layers.Reshape((1, config.hidden_size), name="to_seq")(x)
    x_seq = tf.keras.layers.GRU(
        config.hidden_size, return_sequences=True, return_state=False, reset_after=True, name="gru1"
    )(x_seq)
    x_seq = tf.keras.layers.GRU(
        config.hidden_size, return_sequences=False, return_state=False, reset_after=True, name="gru2"
    )(x_seq)
    hk = tf.keras.layers.Dense(config.filters_flat_len, name="filter_fc")(x_seq)
    return tf.keras.Model(inputs=pk_in, outputs=hk, name="dsenet_filter_estimator")


def build_streaming_step_model(config: DSENetConfig) -> tf.keras.Model:
    """
    Single-step streaming model with explicit recurrent state tensors.
    Inputs:
      p_k: [B, feature_len]
      gru1_state_in: [B, H]
      gru2_state_in: [B, H]
    Outputs:
      h_k: [B, M*(1+Lp+Lf)]
      gru1_state_out: [B, H]
      gru2_state_out: [B, H]
    """
    pk_in = tf.keras.Input(shape=(config.feature_len,), name="p_k")
    gru1_state_in = tf.keras.Input(shape=(config.hidden_size,), name="gru1_state_in")
    gru2_state_in = tf.keras.Input(shape=(config.hidden_size,), name="gru2_state_in")

    x = tf.keras.layers.Lambda(lambda t: _l2_normalize(t, config.l2_eps), name="l2norm")(pk_in)
    x = tf.keras.layers.Dense(config.hidden_size, name="feature_fc")(x)
    x = tf.keras.layers.PReLU(shared_axes=None, name="feature_prelu")(x)
    x = tf.keras.layers.LayerNormalization(epsilon=config.layer_norm_eps, name="feature_ln")(x)

    x_seq = tf.keras.layers.Reshape((1, config.hidden_size), name="to_seq")(x)
    gru1 = tf.keras.layers.GRU(
        config.hidden_size, return_sequences=True, return_state=True, reset_after=True, name="gru1"
    )
    gru2 = tf.keras.layers.GRU(
        config.hidden_size, return_sequences=False, return_state=True, reset_after=True, name="gru2"
    )

    x1_seq, gru1_state_out = gru1(x_seq, initial_state=[gru1_state_in])
    x2, gru2_state_out = gru2(x1_seq, initial_state=[gru2_state_in])
    hk = tf.keras.layers.Dense(config.filters_flat_len, name="filter_fc")(x2)

    return tf.keras.Model(
        inputs=[pk_in, gru1_state_in, gru2_state_in],
        outputs=[hk, gru1_state_out, gru2_state_out],
        name="dsenet_streaming_step",
    )
