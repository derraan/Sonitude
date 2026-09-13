from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
import soundfile as sf

from .dataset import DatasetConfig, paper_target_from_reference_signals

try:
    import pyroomacoustics as pra
except Exception as exc:  # pragma: no cover - dependency gated at runtime
    pra = None
    _PRA_IMPORT_ERROR = exc
else:
    _PRA_IMPORT_ERROR = None


PIXEL3_MIC_POS_CM = np.array(
    [
        [5.1, -1.9, 0.0],
        [4.1, 0.9, 0.0],
        [-9.2, 1.0, 0.0],
    ],
    dtype=np.float64,
)


@dataclass(frozen=True)
class GenerationConfig:
    sample_rate_hz: int = 16000
    duration_s: float = 4.0
    room_l_min_m: float = 5.0
    room_l_max_m: float = 10.0
    room_h_min_m: float = 2.0
    room_h_max_m: float = 4.0
    rt60_min_s: float = 0.1
    rt60_max_s: float = 0.5
    target_az_min_deg: float = -10.0
    target_az_max_deg: float = 10.0
    masker_az_min_deg: float = -180.0
    masker_az_max_deg: float = 180.0
    source_r_min_m: float = 0.5
    source_r_max_m: float = 2.0
    sir_min_db: float = -5.0
    sir_max_db: float = 5.0
    sigma: float = 0.2
    rho: float = 8.0


def _resample_linear(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    if sr_in == sr_out:
        return x.astype(np.float32)
    t_in = np.arange(x.shape[0], dtype=np.float64) / float(sr_in)
    n_out = int(round(x.shape[0] * (float(sr_out) / float(sr_in))))
    t_out = np.arange(n_out, dtype=np.float64) / float(sr_out)
    return np.interp(t_out, t_in, x.astype(np.float64)).astype(np.float32)


def _load_mono(path: Path, sample_rate_hz: int, target_len: int) -> np.ndarray:
    x, sr = sf.read(str(path), dtype="float32")
    if x.ndim > 1:
        x = np.mean(x, axis=1)
    x = _resample_linear(x.astype(np.float32), sr, sample_rate_hz)
    if x.shape[0] < target_len:
        reps = int(np.ceil(target_len / x.shape[0]))
        x = np.tile(x, reps)
    return x[:target_len]


def _polar_to_room_xyz(r: float, az_deg: float, origin_xyz: np.ndarray) -> np.ndarray:
    az = np.deg2rad(az_deg)
    return origin_xyz + np.array([r * np.sin(az), r * np.cos(az), 0.0], dtype=np.float64)


def _simulate_mixture(
    cfg: GenerationConfig, speech_a: np.ndarray, speech_b: np.ndarray, rng: np.random.Generator
) -> tuple[np.ndarray, np.ndarray]:
    if pra is None:
        raise RuntimeError(f"pyroomacoustics import failed: {_PRA_IMPORT_ERROR}")

    for _ in range(20):
        room_l = rng.uniform(cfg.room_l_min_m, cfg.room_l_max_m)
        room_w = rng.uniform(cfg.room_l_min_m, cfg.room_l_max_m)
        room_h = rng.uniform(cfg.room_h_min_m, cfg.room_h_max_m)
        rt60 = rng.uniform(cfg.rt60_min_s, cfg.rt60_max_s)
        try:
            e_abs, max_order = pra.inverse_sabine(rt60, [room_l, room_w, room_h])
            break
        except ValueError:
            continue
    else:
        raise RuntimeError("Could not sample valid room parameters for inverse_sabine")

    room = pra.ShoeBox([room_l, room_w, room_h], fs=cfg.sample_rate_hz, materials=pra.Material(e_abs), max_order=max_order)
    array_center = np.array([room_l / 2.0, room_w / 2.0, 1.5], dtype=np.float64)
    mic_xyz = (PIXEL3_MIC_POS_CM / 100.0) + array_center[None, :]
    room.add_microphone_array(mic_xyz.T)

    target_az = rng.uniform(cfg.target_az_min_deg, cfg.target_az_max_deg)
    masker_az = rng.uniform(cfg.masker_az_min_deg, cfg.masker_az_max_deg)
    target_r = rng.uniform(cfg.source_r_min_m, cfg.source_r_max_m)
    masker_r = rng.uniform(cfg.source_r_min_m, cfg.source_r_max_m)

    target_xyz = _polar_to_room_xyz(target_r, target_az, array_center)
    masker_xyz = _polar_to_room_xyz(masker_r, masker_az, array_center)
    room.add_source(target_xyz, signal=speech_a.astype(np.float32))
    room.add_source(masker_xyz, signal=speech_b.astype(np.float32))
    room.simulate()

    mix = room.mic_array.signals.T.astype(np.float32)  # [T, M]
    # Approximate paper's Eq. (2) target via separately simulated source refs.
    room_target = pra.ShoeBox([room_l, room_w, room_h], fs=cfg.sample_rate_hz, materials=pra.Material(e_abs), max_order=max_order)
    room_target.add_microphone_array(mic_xyz.T)
    room_target.add_source(target_xyz, signal=speech_a.astype(np.float32))
    room_target.simulate()
    x1_target = room_target.mic_array.signals[0].astype(np.float32)

    room_masker = pra.ShoeBox([room_l, room_w, room_h], fs=cfg.sample_rate_hz, materials=pra.Material(e_abs), max_order=max_order)
    room_masker.add_microphone_array(mic_xyz.T)
    room_masker.add_source(masker_xyz, signal=speech_b.astype(np.float32))
    room_masker.simulate()
    x1_masker = room_masker.mic_array.signals[0].astype(np.float32)

    sir_db = rng.uniform(cfg.sir_min_db, cfg.sir_max_db)
    scale = 10.0 ** (-sir_db / 20.0)
    x1_masker *= scale
    mix[:, :] *= 1.0

    min_len = min(mix.shape[0], x1_target.shape[0], x1_masker.shape[0])
    mix = mix[:min_len]
    x1_target = x1_target[:min_len]
    x1_masker = x1_masker[:min_len]

    target = paper_target_from_reference_signals(
        np.stack([x1_target, x1_masker], axis=0),
        np.array([np.deg2rad(target_az), np.deg2rad(masker_az)], dtype=np.float32),
        sigma=cfg.sigma,
        rho=cfg.rho,
    )
    return mix, target


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate DSENet synthetic dataset from paper distributions.")
    parser.add_argument("--librispeech-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--num-samples", type=int, default=64)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--index-offset", type=int, default=0)
    parser.add_argument("--split-name", type=str, default="unspecified")
    args = parser.parse_args()

    if pra is None:
        raise RuntimeError(
            "pyroomacoustics is required for dataset generation. "
            "Install python/dsenet/requirements.txt first."
        )

    files = sorted(args.librispeech_root.rglob("*.flac"))
    if len(files) < 2:
        raise RuntimeError("Need at least two LibriSpeech files")

    cfg = GenerationConfig()
    ds_cfg = DatasetConfig(sample_rate_hz=cfg.sample_rate_hz, duration_s=cfg.duration_s, sigma=cfg.sigma, rho=cfg.rho)
    n = int(round(ds_cfg.sample_rate_hz * ds_cfg.duration_s))
    rng = np.random.default_rng(args.seed)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    manifest = {"config": asdict(cfg), "samples": []}
    for idx in range(args.num_samples):
        ridx = idx + max(0, args.index_offset)
        a = files[(2 * ridx) % len(files)]
        b = files[(2 * ridx + 1) % len(files)]
        xa = _load_mono(a, cfg.sample_rate_hz, n)
        xb = _load_mono(b, cfg.sample_rate_hz, n)
        mix, target = _simulate_mixture(cfg, xa, xb, rng)
        mix_path = args.output_dir / f"mix_{idx:05d}.npy"
        tgt_path = args.output_dir / f"target_{idx:05d}.npy"
        np.save(mix_path, mix)
        np.save(tgt_path, target)
        manifest["samples"].append(
            {
                "id": f"{args.split_name}_{idx:05d}",
                "mixture_npy": mix_path.name,
                "target_npy": tgt_path.name,
                "src_a": str(a),
                "src_b": str(b),
            }
        )

    manifest["split"] = args.split_name
    (args.output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("Generated", args.num_samples, "samples at", args.output_dir)


if __name__ == "__main__":
    main()
