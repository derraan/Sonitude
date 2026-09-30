"""Reusable LibriSpeech distance→dBFS sweep for training validation and CLI eval."""
from __future__ import annotations

import math
import os
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import librosa
import numpy as np
import pyroomacoustics as pra
import torch

SR = 24000
DUR_S = 2.0
TARGET_IN_DBFS = -25.0
DEFAULT_DISTANCES = np.round(np.arange(3.0, 0.49, -0.1), 2)
DEFAULT_RT60_LIST = [0.2, 0.8, 1.0, 1.6]
DEFAULT_SPEECH_PATH = r"C:\Users\darre\Sound_Bubble\datasets\_speech_cache\5694-64038-0000.flac"

MIC_OFFSETS_M = np.array(
    [
        [-0.128, -0.015, 0.0],
        [-0.102, 0.0, 0.113],
        [-0.038, 0.0, 0.169],
        [0.038, 0.0, 0.169],
        [0.106, 0.0, 0.117],
        [0.131, -0.015, 0.007],
    ],
    dtype=np.float64,
)


def db_fs(x: np.ndarray) -> float:
    x = np.asarray(x, dtype=np.float64).reshape(-1)
    rms = float(np.sqrt(np.mean(x * x) + 1e-12))
    return 20.0 * math.log10(max(rms, 1e-12))


def load_libri_segment(path: str, sr: int = SR, dur_s: float = DUR_S) -> np.ndarray:
    y, _ = librosa.load(path, sr=sr, mono=True)
    n = int(sr * dur_s)
    if len(y) < n:
        reps = int(np.ceil(n / max(len(y), 1)))
        y = np.tile(y, reps)
    y = y[:n].astype(np.float32)
    peak = float(np.max(np.abs(y)) + 1e-12)
    return (y / peak * 0.3).astype(np.float32)


def simulate_at_distance(distance_m: float, src: np.ndarray, sr: int, rt60_s: float) -> np.ndarray:
    room_dim = [8.0, 8.0, 3.0]
    mic_center = np.array([4.0, 4.0, 1.5])
    mics = MIC_OFFSETS_M + mic_center
    src_pos = mic_center + np.array([0.0, distance_m, 0.0])
    src_pos = np.clip(src_pos, [0.3, 0.3, 0.3], [room_dim[0] - 0.3, room_dim[1] - 0.3, room_dim[2] - 0.3])

    e_absorption, max_order = pra.inverse_sabine(rt60_s, room_dim)
    max_order = int(min(max_order, 80))
    room = pra.ShoeBox(
        room_dim,
        fs=sr,
        materials=pra.Material(e_absorption),
        max_order=max_order,
    )
    room.add_microphone_array(mics.T)
    room.add_source(src_pos, signal=src.astype(np.float64))
    room.simulate()
    mix = np.asarray(room.mic_array.signals, dtype=np.float32)
    n = len(src)
    if mix.shape[1] < n:
        mix = np.concatenate([mix, np.zeros((6, n - mix.shape[1]), dtype=np.float32)], axis=1)
    else:
        mix = mix[:, :n]
    return mix


def normalize_mic0_level(mix: np.ndarray, target_dbfs: float) -> np.ndarray:
    cur = db_fs(mix[0])
    gain = 10.0 ** ((target_dbfs - cur) / 20.0)
    return (mix * gain).astype(np.float32)


def _as_float_list(values: Optional[Sequence[float]], default: Sequence[float]) -> List[float]:
    if values is None:
        return [float(v) for v in default]
    return [float(v) for v in values]


@torch.no_grad()
def run_distance_sweep(
    model: torch.nn.Module,
    device: str,
    *,
    speech_path: str = DEFAULT_SPEECH_PATH,
    distances: Optional[Sequence[float]] = None,
    rt60_list: Optional[Sequence[float]] = None,
    target_in_dbfs: float = TARGET_IN_DBFS,
    sr: int = SR,
    dur_s: float = DUR_S,
) -> Dict[str, object]:
    """Run model on distance×RT60 grid; return levels and compact summary metrics."""
    distances_f = _as_float_list(distances, DEFAULT_DISTANCES)
    rt60_f = _as_float_list(rt60_list, DEFAULT_RT60_LIST)
    src = load_libri_segment(speech_path, sr=sr, dur_s=dur_s)

    results: Dict[float, List[float]] = {}
    input_levels: List[float] = []

    was_training = model.training
    model.eval()
    try:
        for rt60 in rt60_f:
            outs: List[float] = []
            for i, d in enumerate(distances_f):
                mix = simulate_at_distance(float(d), src, sr, float(rt60))
                mix = normalize_mic0_level(mix, float(target_in_dbfs))
                if rt60 == rt60_f[0]:
                    input_levels.append(db_fs(mix[0]))
                mix_t = torch.from_numpy(mix).unsqueeze(0).to(device)
                out = model({"mixture": mix_t})["output"].squeeze(0).detach().cpu().numpy()
                if out.ndim > 1:
                    out = out[0]
                outs.append(db_fs(out))
            results[float(rt60)] = outs
    finally:
        if was_training:
            model.train()

    # Mean across RT60 at each distance
    mean_out = []
    for i in range(len(distances_f)):
        mean_out.append(float(np.mean([results[rt][i] for rt in results])))

    def _level_at(dist_m: float) -> float:
        # nearest grid point
        idx = int(np.argmin(np.abs(np.asarray(distances_f) - dist_m)))
        return mean_out[idx]

    summary = {
        "out_dbfs_0_5m": _level_at(0.5),
        "out_dbfs_0_8m": _level_at(0.8),
        "out_dbfs_1_0m": _level_at(1.0),
        "out_dbfs_1_2m": _level_at(1.2),
        "out_dbfs_1_5m": _level_at(1.5),
        "out_dbfs_2_0m": _level_at(2.0),
        "out_dbfs_3_0m": _level_at(3.0),
        # Positive = passband louder than far field (desired shape).
        "passband_minus_far_db": _level_at(0.8) - _level_at(3.0),
        "edge_drop_1_5_to_2_0_db": _level_at(1.5) - _level_at(2.0),
    }

    return {
        "distances_m": distances_f,
        "rt60_list": rt60_f,
        "input_mic0_dbfs": input_levels,
        "out_dbfs_by_rt60": results,
        "out_dbfs_mean": mean_out,
        "summary": summary,
    }


def write_sweep_csv(path: str, sweep: Dict[str, object]) -> None:
    distances = sweep["distances_m"]  # type: ignore[index]
    results = sweep["out_dbfs_by_rt60"]  # type: ignore[index]
    input_levels = sweep["input_mic0_dbfs"]  # type: ignore[index]
    rt60_list = sweep["rt60_list"]  # type: ignore[index]
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        header = "distance_m," + ",".join([f"out_rt60_{rt60}" for rt60 in rt60_list]) + ",input_mic0_dbfs\n"
        f.write(header)
        for i, d in enumerate(distances):
            row = [f"{d}"] + [f"{results[rt60][i]:.4f}" for rt60 in rt60_list] + [f"{input_levels[i]:.4f}"]
            f.write(",".join(row) + "\n")
