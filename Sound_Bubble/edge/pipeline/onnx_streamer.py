from __future__ import annotations

import time
from dataclasses import dataclass

import numpy as np
import onnxruntime as ort


DIS_EMBED_ONEHOT: dict[float, tuple[float, float, float]] = {
    1.0: (0.0, 0.0, 1.0),
    1.5: (0.0, 1.0, 0.0),
    2.0: (1.0, 0.0, 0.0),
}


@dataclass(frozen=True)
class OnnxRunResult:
    output: np.ndarray
    infer_ms: float


class ONNXStreamer:
    def __init__(self, model_path: str, intra_threads: int, inter_threads: int = 1) -> None:
        options = ort.SessionOptions()
        options.intra_op_num_threads = int(intra_threads)
        options.inter_op_num_threads = int(inter_threads)
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        self.session = ort.InferenceSession(model_path, sess_options=options, providers=["CPUExecutionProvider"])
        self.input_names = [x.name for x in self.session.get_inputs()]
        self.output_names = [x.name for x in self.session.get_outputs()]
        self._output_name_to_index = {name: i for i, name in enumerate(self.output_names)}
        if "output" not in self.output_names:
            raise RuntimeError("Expected ONNX output named 'output'.")
        self._output_index = self._output_name_to_index["output"]
        if "mixture" not in self.input_names:
            raise RuntimeError("Expected ONNX input named 'mixture'.")
        self.accepts_dis_embed = "dis_embed" in self.input_names
        self.state_input_names = sorted(
            [x for x in self.input_names if x.startswith("state_")], key=lambda n: int(n.split("_")[1])
        )
        self.state_output_names = sorted(
            [x for x in self.output_names if x.startswith("next_state_")], key=lambda n: int(n.split("_")[2])
        )
        if len(self.state_input_names) != len(self.state_output_names):
            raise RuntimeError("State input/output count mismatch in ONNX graph.")
        missing = [name for name in self.state_output_names if name not in self._output_name_to_index]
        if missing:
            raise RuntimeError(f"Expected ONNX state outputs not found: {missing}")
        self._state_output_indices = [self._output_name_to_index[name] for name in self.state_output_names]
        self.state_arrays: dict[str, np.ndarray] = {}
        for meta in self.session.get_inputs():
            if not meta.name.startswith("state_"):
                continue
            shape = []
            for dim in meta.shape:
                if isinstance(dim, int) and dim > 0:
                    shape.append(dim)
                else:
                    shape.append(1)
            self.state_arrays[meta.name] = np.zeros(shape, dtype=np.float32)
        self._dis_embed: np.ndarray | None = None

    def reset_state(self) -> None:
        for v in self.state_arrays.values():
            v.fill(0.0)

    def reset_state_and_warm(self) -> None:
        self.reset_state()

    def set_radius(self, radius: float) -> None:
        if not self.accepts_dis_embed:
            return
        r = float(radius)
        if r not in DIS_EMBED_ONEHOT:
            raise ValueError(f"Bubble radius {r} not in training set {sorted(DIS_EMBED_ONEHOT)}.")
        self._dis_embed = np.array([DIS_EMBED_ONEHOT[r]], dtype=np.float32)

    def run(self, mixture: np.ndarray) -> OnnxRunResult:
        feeds: dict[str, np.ndarray] = {"mixture": mixture.astype(np.float32, copy=False)}
        if self.accepts_dis_embed:
            if self._dis_embed is None:
                raise RuntimeError("ONNX expects dis_embed but radius was not set. Call set_radius() first.")
            feeds["dis_embed"] = self._dis_embed
        for name in self.state_input_names:
            feeds[name] = self.state_arrays[name]
        t0 = time.perf_counter()
        outputs = self.session.run(self.output_names, feeds)
        infer_ms = (time.perf_counter() - t0) * 1000.0
        for in_name, out_idx in zip(self.state_input_names, self._state_output_indices):
            self.state_arrays[in_name] = outputs[out_idx].astype(np.float32, copy=False)
        return OnnxRunResult(output=outputs[self._output_index], infer_ms=infer_ms)

