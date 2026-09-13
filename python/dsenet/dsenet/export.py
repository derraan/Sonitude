from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import asdict
from pathlib import Path

import numpy as np
import tensorflow as tf

from .model import DSENetConfig, build_streaming_step_model


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            block = f.read(1 << 20)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export DSENet streaming step model to float32 TFLite.")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--sample-rate-hz", type=int, default=16000)
    parser.add_argument("--num-mics", type=int, default=3)
    parser.add_argument("--hop-size", type=int, default=32)
    parser.add_argument("--lookback", type=int, default=32)
    parser.add_argument("--lookahead", type=int, default=32)
    parser.add_argument("--hidden-size", type=int, default=128)
    parser.add_argument("--eta", type=float, default=1.0)
    parser.add_argument("--reference-mic-index", type=int, default=0)
    parser.add_argument("--trained", action="store_true")
    parser.add_argument("--geometry-id", action="append", default=[])
    parser.add_argument(
        "--geometry-xyz",
        action="append",
        default=[],
        help="Repeat as x,y,z for each mic in metadata order",
    )
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
    if len(args.geometry_id) != cfg.num_mics or len(args.geometry_xyz) != cfg.num_mics:
        raise ValueError(
            "Provide exactly num_mics --geometry-id and --geometry-xyz entries for strict runtime validation."
        )
    geometry_xyz = []
    for text in args.geometry_xyz:
        parts = [p.strip() for p in text.split(",")]
        if len(parts) != 3:
            raise ValueError(f"Invalid --geometry-xyz value: {text}")
        geometry_xyz.append([float(parts[0]), float(parts[1]), float(parts[2])])

    model = build_streaming_step_model(cfg)
    model([np.zeros((1, cfg.feature_len), np.float32), np.zeros((1, cfg.hidden_size), np.float32), np.zeros((1, cfg.hidden_size), np.float32)])

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.target_spec.supported_types = [tf.float32]
    tflite_bytes = converter.convert()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    tflite_path = args.out_dir / "dsenet_streaming_step.tflite"
    tflite_path.write_bytes(tflite_bytes)

    metadata = {
        "schema_version": 1,
        "sample_rate_hz": cfg.sample_rate_hz,
        "num_mics": cfg.num_mics,
        "reference_mic_index": args.reference_mic_index,
        "L": cfg.hop_size,
        "Lp": cfg.lookback,
        "Lf": cfg.lookahead,
        "H": cfg.hidden_size,
        "eta": args.eta,
        "trained": bool(args.trained),
        "tensor_names": {
            "p_k": "serving_default_p_k:0",
            "gru1_state_in": "serving_default_gru1_state_in:0",
            "gru2_state_in": "serving_default_gru2_state_in:0",
            "h_k": "StatefulPartitionedCall:0",
            "gru1_state_out": "StatefulPartitionedCall:1",
            "gru2_state_out": "StatefulPartitionedCall:2",
        },
        "tensor_shapes": {
            "p_k": [1, cfg.feature_len],
            "gru1_state": [1, cfg.hidden_size],
            "gru2_state": [1, cfg.hidden_size],
            "h_k": [1, cfg.filters_flat_len],
        },
        "model_sha256": _sha256(tflite_path),
        "model_filename": tflite_path.name,
        "geometry": {"ids": args.geometry_id, "xyz_m": geometry_xyz},
        "config": asdict(cfg),
    }
    metadata_path = args.out_dir / "dsenet_streaming_step.dsenet.json"
    metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print("Wrote:", tflite_path)
    print("Wrote:", metadata_path)


if __name__ == "__main__":
    main()
