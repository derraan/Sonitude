from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import tensorflow as tf

from .model import DSENetConfig, build_streaming_step_model


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify streaming equivalence for DSENet GRU export.")
    parser.add_argument("--tflite", type=Path, required=True)
    parser.add_argument("--hops", type=int, default=1000)
    parser.add_argument("--rtol", type=float, default=1e-5)
    parser.add_argument("--atol", type=float, default=1e-6)
    parser.add_argument("--sample-rate-hz", type=int, default=16000)
    parser.add_argument("--num-mics", type=int, default=3)
    parser.add_argument("--hop-size", type=int, default=32)
    parser.add_argument("--lookback", type=int, default=32)
    parser.add_argument("--lookahead", type=int, default=32)
    parser.add_argument("--hidden-size", type=int, default=128)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    cfg = DSENetConfig(
        sample_rate_hz=args.sample_rate_hz,
        num_mics=args.num_mics,
        hop_size=args.hop_size,
        lookback=args.lookback,
        lookahead=args.lookahead,
        hidden_size=args.hidden_size,
    )
    keras_model = build_streaming_step_model(cfg)
    dummy = [
        np.zeros((1, cfg.feature_len), np.float32),
        np.zeros((1, cfg.hidden_size), np.float32),
        np.zeros((1, cfg.hidden_size), np.float32),
    ]
    keras_model(dummy)

    interpreter = tf.lite.Interpreter(model_path=str(args.tflite))
    interpreter.allocate_tensors()
    in_details = interpreter.get_input_details()
    out_details = interpreter.get_output_details()

    rng = np.random.default_rng(1234)
    s1 = np.zeros((1, cfg.hidden_size), np.float32)
    s2 = np.zeros((1, cfg.hidden_size), np.float32)

    max_abs = 0.0
    for hop in range(args.hops):
        pk = rng.standard_normal((1, cfg.feature_len), dtype=np.float32)

        hk_k, s1_k, s2_k = keras_model([pk, s1, s2], training=False)
        hk_ref = hk_k.numpy()
        s1_ref = s1_k.numpy()
        s2_ref = s2_k.numpy()

        interpreter.set_tensor(in_details[0]["index"], pk)
        interpreter.set_tensor(in_details[1]["index"], s1)
        interpreter.set_tensor(in_details[2]["index"], s2)
        interpreter.invoke()
        hk_tfl = interpreter.get_tensor(out_details[0]["index"])
        s1_tfl = interpreter.get_tensor(out_details[1]["index"])
        s2_tfl = interpreter.get_tensor(out_details[2]["index"])

        np.testing.assert_allclose(hk_tfl, hk_ref, rtol=args.rtol, atol=args.atol)
        np.testing.assert_allclose(s1_tfl, s1_ref, rtol=args.rtol, atol=args.atol)
        np.testing.assert_allclose(s2_tfl, s2_ref, rtol=args.rtol, atol=args.atol)

        max_abs = max(max_abs, float(np.max(np.abs(hk_tfl - hk_ref))))
        s1 = s1_tfl
        s2 = s2_tfl
        if hop == args.hops // 2:
            # Explicit mid-stream reset check.
            s1.fill(0.0)
            s2.fill(0.0)

    print(f"Streaming parity passed over {args.hops} hops. max_abs={max_abs:.6e}")


if __name__ == "__main__":
    main()
