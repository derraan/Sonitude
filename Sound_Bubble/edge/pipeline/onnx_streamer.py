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
    distance_m: float | None = None
    distance_status: str = "unavailable"
    distance_valid: float | None = None
    speaker_count: str | None = None
    count_confidence: float | None = None
    count_status: str = "unavailable"
    speaker_distances: tuple[dict[str, object], ...] | None = None


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
        self._distance_m_index = self._output_name_to_index.get("distance_m")
        self._distance_valid_index = self._output_name_to_index.get("distance_valid")
        self.has_distance_outputs = self._distance_m_index is not None and self._distance_valid_index is not None
        self._count_probs_index = self._output_name_to_index.get("count_probs")
        self.has_count_outputs = self._count_probs_index is not None
        self.distance_valid_threshold = 0.5
        self.distance_trained = False
        self.count_min_confidence = 0.6
        self.count_trained = False
        self.gate_distance_on_count = False
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

    def set_distance_runtime_config(
        self,
        *,
        trained: bool | None = None,
        valid_threshold: float | None = None,
        gate_on_count: bool | None = None,
    ) -> None:
        if trained is not None:
            self.distance_trained = bool(trained)
        if valid_threshold is not None:
            self.distance_valid_threshold = float(valid_threshold)
        if gate_on_count is not None:
            self.gate_distance_on_count = bool(gate_on_count)

    def set_count_runtime_config(
        self,
        *,
        trained: bool | None = None,
        min_confidence: float | None = None,
    ) -> None:
        if trained is not None:
            self.count_trained = bool(trained)
        if min_confidence is not None:
            self.count_min_confidence = float(min_confidence)

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
        distance_status = "unavailable"
        distance_m = None
        distance_valid = None
        distance_slots: list[tuple[float, float]] = []
        if self.has_distance_outputs and self.distance_trained:
            distance_arr = outputs[self._distance_m_index]
            valid_arr = outputs[self._distance_valid_index]
            if distance_arr.ndim == 3:
                distance_vals = distance_arr[0, -1, :]
                valid_vals = valid_arr[0, -1, :]
            elif distance_arr.ndim == 2:
                distance_vals = np.array([distance_arr[0, -1]], dtype=np.float32)
                valid_vals = np.array([valid_arr[0, -1]], dtype=np.float32)
            else:
                distance_vals = np.array([distance_arr.reshape(-1)[-1]], dtype=np.float32)
                valid_vals = np.array([valid_arr.reshape(-1)[-1]], dtype=np.float32)

            for d_val, v_val in zip(distance_vals, valid_vals):
                distance_slots.append((float(d_val), float(v_val)))

            has_valid = False
            for d_val, v_val in distance_slots:
                if np.isfinite(d_val) and np.isfinite(v_val) and v_val >= self.distance_valid_threshold and d_val > 0.0:
                    distance_m = d_val
                    distance_valid = v_val
                    distance_status = "valid"
                    has_valid = True
                    break
            if not has_valid:
                distance_valid = float(distance_slots[0][1]) if len(distance_slots) > 0 else None
                distance_status = "inactive"

        count_status = "unavailable"
        speaker_count = None
        count_confidence = None
        if self.has_count_outputs and self.count_trained:
            count_arr = outputs[self._count_probs_index]
            count_vec = count_arr[0, -1, :] if count_arr.ndim == 3 else count_arr.reshape(-1)[-3:]
            pred_idx = int(np.argmax(count_vec))
            pred_conf = float(count_vec[pred_idx])
            count_confidence = pred_conf
            if pred_conf >= self.count_min_confidence:
                count_status = "valid"
                speaker_count = ("0", "1", "2+")[pred_idx]
            else:
                count_status = "low_confidence"

        if self.gate_distance_on_count and count_status == "valid" and speaker_count != "1":
            distance_status = "inactive"
            distance_m = None

        speaker_distances = None
        if count_status == "valid" and speaker_count is not None:
            n_speakers = 0 if speaker_count == "0" else (1 if speaker_count == "1" else 2)
            rows: list[dict[str, object]] = []
            for idx in range(n_speakers):
                row_status = "unavailable"
                row_distance = None
                if len(distance_slots) > idx:
                    d_val, v_val = distance_slots[idx]
                    if np.isfinite(d_val) and np.isfinite(v_val) and v_val >= self.distance_valid_threshold and d_val > 0.0:
                        row_status = "valid"
                        row_distance = d_val
                    else:
                        row_status = "inactive"
                elif idx == 0 and distance_status in ("valid", "inactive"):
                    row_status = distance_status
                    row_distance = distance_m
                rows.append(
                    {
                        "speaker_index": idx + 1,
                        "distance_m": row_distance,
                        "status": row_status,
                    }
                )
            speaker_distances = tuple(rows)
        return OnnxRunResult(
            output=outputs[self._output_index],
            infer_ms=infer_ms,
            distance_m=distance_m,
            distance_status=distance_status,
            distance_valid=distance_valid,
            speaker_count=speaker_count,
            count_confidence=count_confidence,
            count_status=count_status,
            speaker_distances=speaker_distances,
        )

