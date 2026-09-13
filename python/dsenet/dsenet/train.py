from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import tensorflow as tf

from .model import DSENetConfig, build_dsenet_filter_estimator


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="DSENet training entrypoint (no auto-run dataset jobs).")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--sample-rate-hz", type=int, default=16000)
    parser.add_argument("--num-mics", type=int, default=3)
    parser.add_argument("--hop-size", type=int, default=32)
    parser.add_argument("--lookback", type=int, default=32)
    parser.add_argument("--lookahead", type=int, default=32)
    parser.add_argument("--hidden-size", type=int, default=128)
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--learning-rate", type=float, default=1e-3)
    parser.add_argument("--dry-run", action="store_true")
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

    model = build_dsenet_filter_estimator(cfg)
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=args.learning_rate),
        loss="mse",
    )

    # Placeholder tensors for pipeline verification; real dataset generation is external.
    x = np.zeros((args.batch_size, cfg.feature_len), dtype=np.float32)
    y = np.zeros((args.batch_size, cfg.filters_flat_len), dtype=np.float32)

    if not args.dry_run:
        lr_schedule = tf.keras.callbacks.LearningRateScheduler(
            lambda epoch, lr: lr * (0.98 if (epoch > 0 and epoch % 2 == 0) else 1.0)
        )
        model.fit(x, y, epochs=args.epochs, batch_size=args.batch_size, callbacks=[lr_schedule], verbose=1)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    model.save(args.output_dir / "dsenet_filter_estimator.keras")
    print("Saved:", args.output_dir / "dsenet_filter_estimator.keras")


if __name__ == "__main__":
    main()
