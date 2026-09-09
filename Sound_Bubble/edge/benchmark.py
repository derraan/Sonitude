from __future__ import annotations

import argparse
import time

import numpy as np
import onnxruntime as ort

from runtime_contract import load_contract


DEFAULT_WARMUP = 20
DEFAULT_RUNS = 100
DEFAULT_THREADS = 4
DEFAULT_BATCH = 1
DEFAULT_CHANNELS = 6
DEFAULT_FRAME_LEN = 288


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Benchmark Sound_Bubble ONNX inference latency.")
    parser.add_argument("--model", type=str, required=True, help="Path to ONNX model.")
    parser.add_argument("--warmup", type=int, default=DEFAULT_WARMUP, help="Warmup forward passes.")
    parser.add_argument("--runs", type=int, default=DEFAULT_RUNS, help="Timed benchmark runs.")
    parser.add_argument("--num-threads", type=int, default=DEFAULT_THREADS, help="CPU thread count for ORT intra-op.")
    parser.add_argument("--batch-size", type=int, default=DEFAULT_BATCH, help="Batch size for dummy input.")
    parser.add_argument("--channels", type=int, default=DEFAULT_CHANNELS, help="Channel count for mixture input.")
    parser.add_argument("--frame-len", type=int, default=DEFAULT_FRAME_LEN, help="Frame length for mixture input.")
    parser.add_argument("--contract-path", type=str, default=None, help="Runtime contract JSON produced during export.")
    return parser


def _shape_from_ort(meta_shape: list[int | str | None], batch: int, channels: int, frame_len: int) -> list[int]:
    resolved: list[int] = []
    for idx, dim in enumerate(meta_shape):
        if isinstance(dim, int) and dim > 0:
            resolved.append(dim)
        else:
            if idx == 0:
                resolved.append(batch)
            else:
                resolved.append(1)
    return resolved


def main() -> None:
    args = _build_parser().parse_args()
    if args.contract_path:
        contract = load_contract(args.contract_path)
        args.channels = contract.model_num_ch
        args.frame_len = contract.frame_len
    options = ort.SessionOptions()
    options.intra_op_num_threads = int(args.num_threads)
    options.inter_op_num_threads = 1
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL

    session = ort.InferenceSession(args.model, sess_options=options, providers=["CPUExecutionProvider"])
    input_metas = session.get_inputs()
    output_metas = session.get_outputs()

    if not input_metas:
        raise RuntimeError("ONNX model has no inputs.")

    feeds: dict[str, np.ndarray] = {}
    for input_meta in input_metas:
        if input_meta.name == "mixture":
            shape = [args.batch_size, args.channels, args.frame_len]
        else:
            shape = _shape_from_ort(input_meta.shape, args.batch_size, args.channels, args.frame_len)
        feeds[input_meta.name] = np.zeros(shape, dtype=np.float32)

    print(f"[info] model={args.model}")
    print(f"[info] provider=CPUExecutionProvider, intra_op={options.intra_op_num_threads}, inter_op=1")
    print(f"[info] warmup={args.warmup}, runs={args.runs}")

    state_input_names = sorted([m.name for m in input_metas if m.name.startswith("state_")], key=lambda n: int(n.split("_")[1]))
    state_output_names = sorted(
        [m.name for m in output_metas if m.name.startswith("next_state_")],
        key=lambda n: int(n.split("_")[2]),
    )
    if len(state_input_names) != len(state_output_names):
        raise RuntimeError("State input/output count mismatch.")

    for _ in range(args.warmup):
        outputs = session.run(None, feeds)
        out_map = dict(zip([m.name for m in output_metas], outputs))
        for in_name, out_name in zip(state_input_names, state_output_names):
            feeds[in_name] = out_map[out_name].astype(np.float32, copy=False)

    timings_ms: list[float] = []
    for _ in range(args.runs):
        t0 = time.perf_counter()
        outputs = session.run(None, feeds)
        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        timings_ms.append(elapsed_ms)
        out_map = dict(zip([m.name for m in output_metas], outputs))
        for in_name, out_name in zip(state_input_names, state_output_names):
            feeds[in_name] = out_map[out_name].astype(np.float32, copy=False)

    mean_ms = float(np.mean(timings_ms))
    std_ms = float(np.std(timings_ms))
    p95_ms = float(np.percentile(timings_ms, 95.0))
    print(f"[result] inference mean={mean_ms:.3f} ms  std={std_ms:.3f} ms  p95={p95_ms:.3f} ms")


if __name__ == "__main__":
    main()
