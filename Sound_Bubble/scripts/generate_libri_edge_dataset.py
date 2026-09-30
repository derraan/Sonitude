"""
Generate a LibriSpeech-based synthetic dataset for stopband nudging.

Outputs per sample:
- mixture.wav                (6-channel mixture)
- mic00_voiceXX.wav          (reference mic target for each inside-bubble voice)
- metadata.json              (voice distances + mic geometry)

Worktree-local for the stopband experiment. Uses pyroomacoustics shoebox rooms
so edge scenes match reverberant syn_1_5m more closely than direct-path rendering.
"""
from __future__ import annotations

import argparse
import json
import math
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Sequence

import librosa
import numpy as np
import pyroomacoustics as pra
import soundfile as sf
from tqdm import tqdm


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


@dataclass(frozen=True)
class VoiceSource:
    path: Path
    speaker_id: str
    wav: np.ndarray


def _make_segment(wav: np.ndarray, n_samples: int) -> np.ndarray:
    if wav.shape[0] >= n_samples:
        start = np.random.randint(0, wav.shape[0] - n_samples + 1)
        seg = wav[start : start + n_samples]
    else:
        reps = int(math.ceil(n_samples / max(1, wav.shape[0])))
        seg = np.tile(wav, reps)[:n_samples]
    ramp = min(400, n_samples // 8)
    if ramp > 0:
        w = np.ones(n_samples, dtype=np.float32)
        r = np.linspace(0.0, 1.0, ramp, dtype=np.float32)
        w[:ramp] *= r
        w[-ramp:] *= r[::-1]
        seg = seg * w
    return seg.astype(np.float32)


def _sample_inside_count(p0: float, p1: float) -> int:
    u = random.random()
    if u < p0:
        return 0
    if u < p0 + p1:
        return 1
    return 2


def _sample_room_and_mics() -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    room_x = random.uniform(5.0, 8.0)
    room_y = random.uniform(4.0, 8.0)
    room_z = random.uniform(2.4, 3.2)
    room_dim = np.array([room_x, room_y, room_z], dtype=np.float64)
    cx = random.uniform(1.0, room_x - 1.0)
    cy = random.uniform(1.0, room_y - 1.0)
    cz = random.uniform(1.2, min(1.8, room_z - 0.4))
    center = np.array([cx, cy, cz], dtype=np.float64)
    yaw = random.uniform(-math.pi, math.pi)
    rot = np.array(
        [
            [math.cos(yaw), -math.sin(yaw), 0.0],
            [math.sin(yaw), math.cos(yaw), 0.0],
            [0.0, 0.0, 1.0],
        ],
        dtype=np.float64,
    )
    mics = (MIC_OFFSETS_M @ rot.T) + center
    return room_dim, center, mics


def _sample_source_position(
    center: np.ndarray,
    room_dim: np.ndarray,
    *,
    dis_threshold: float,
    outside_gap: float,
    inside: bool,
) -> tuple[np.ndarray, float]:
    r_min = 0.35
    # Keep all candidate points within wall safety margins for any angle.
    r_max_geom = min(
        center[0] - 0.35,
        room_dim[0] - 0.35 - center[0],
        center[1] - 0.35,
        room_dim[1] - 0.35 - center[1],
    )
    r_max_geom = max(r_min + 0.05, float(r_max_geom))
    if inside:
        r_low = r_min
        r_high = max(r_min + 0.05, min(dis_threshold - 0.03, r_max_geom))
    else:
        r_low = min(dis_threshold + outside_gap, r_max_geom - 0.05)
        r_high = max(r_low + 0.05, r_max_geom)
    r = random.uniform(r_low, r_high)
    th = random.uniform(-math.pi, math.pi)
    x = center[0] + r * math.cos(th)
    y = center[1] + r * math.sin(th)
    z = random.uniform(max(0.8, center[2] - 0.25), center[2] + 0.25)
    x = float(np.clip(x, 0.35, room_dim[0] - 0.35))
    y = float(np.clip(y, 0.35, room_dim[1] - 0.35))
    z = float(np.clip(z, 0.5, room_dim[2] - 0.35))
    pos = np.array([x, y, z], dtype=np.float64)
    r_exact = float(np.linalg.norm(pos[:2] - center[:2]))
    return pos, r_exact


def _render_pyroom(
    segments: Sequence[np.ndarray],
    positions: Sequence[np.ndarray],
    mic_positions: np.ndarray,
    room_dim: np.ndarray,
    sr: int,
    n_samples: int,
    target_rt60: Optional[float] = None,
) -> tuple[list[np.ndarray], float, float, int]:
    """Return per-source [n_mics, T] tracks, rt60, absorption, max_order."""
    if target_rt60 is not None and target_rt60 > 0:
        e_absorption, max_order = pra.inverse_sabine(float(target_rt60), room_dim.tolist())
        absorption = float(e_absorption)
        max_order = int(min(max(int(max_order), 8), 80))
    else:
        absorption = float(random.uniform(0.2, 0.85))
        max_order = int(random.randint(8, 24))
    room = pra.ShoeBox(
        p=room_dim.tolist(),
        fs=sr,
        materials=pra.Material(absorption),
        max_order=max_order,
    )
    room.add_microphone_array(mic_positions.T)
    for seg, pos in zip(segments, positions):
        room.add_source(pos.tolist(), signal=np.asarray(seg, dtype=np.float64))
    premix = room.simulate(return_premix=True)
    try:
        rt60 = float(np.mean(room.measure_rt60()))
    except Exception:
        rt60 = float(target_rt60) if target_rt60 is not None else float("nan")
    tracks: list[np.ndarray] = []
    n_mics = mic_positions.shape[0]
    for i in range(len(segments)):
        chans = []
        for m in range(n_mics):
            sig = np.asarray(premix[i][m], dtype=np.float32)
            if sig.shape[0] < n_samples:
                sig = np.pad(sig, (0, n_samples - sig.shape[0]))
            chans.append(sig[:n_samples])
        tracks.append(np.stack(chans, axis=0).astype(np.float32))
    return tracks, rt60, absorption, max_order


def _build_metadata(
    *,
    sources: list[dict[str, object]],
    mics: np.ndarray,
    n_in: int,
    n_out: int,
    room_dim: np.ndarray,
    absorption: float,
    max_order: int,
    rt60: float,
) -> dict[str, object]:
    metadata: dict[str, object] = {}
    for i, s in enumerate(sources):
        metadata[f"voice{i:02d}"] = {
            "dis": float(s["distance"]),
            "angle": float(s["angle_deg"]),
            "speaker_id": str(s["speaker_id"]),
            "position": [float(v) for v in s["pos"]],
        }
    for m in range(mics.shape[0]):
        metadata[f"mic{m:02d}"] = {"position": [float(v) for v in mics[m]]}
    metadata["n_in"] = int(n_in)
    metadata["n_out"] = int(n_out)
    metadata["n_BG"] = 0
    metadata["real"] = False
    metadata["room"] = "Synthetic"
    metadata["room_info"] = {
        "walls": [0.0, float(room_dim[0]), float(room_dim[1]), 0.0],
        "absorption": float(absorption),
        "max_order": int(max_order),
        "rt60": float(rt60),
    }
    metadata["input_snr"] = None
    metadata["snr_clipped"] = 0
    return metadata


def _choose_sources(pool: Sequence[VoiceSource], n_total: int) -> list[VoiceSource]:
    if not pool:
        raise RuntimeError("Voice source pool is empty.")
    return [random.choice(pool) for _ in range(n_total)]


def _speaker_id_from_path(path: Path) -> str:
    # LibriSpeech: <speaker>/<chapter>/<speaker>-<chapter>-<utt>.flac
    if path.parent.parent.name.isdigit():
        return path.parent.parent.name
    stem = path.stem
    toks = stem.replace("-", "_").split("_")
    return toks[0] if toks else stem[:8]


def _list_audio_files(speech_dir: Path) -> list[Path]:
    return sorted(list(speech_dir.rglob("*.wav")) + list(speech_dir.rglob("*.flac")))


def _build_voice_pool(
    speech_dir: Path,
    sr: int,
    *,
    max_files: Optional[int],
    seed: int,
    label: str,
) -> list[VoiceSource]:
    wavs = _list_audio_files(speech_dir)
    if not wavs:
        raise RuntimeError(f"No wav/flac files found under: {speech_dir}")
    if max_files is not None and max_files > 0 and len(wavs) > max_files:
        rng = random.Random(seed)
        wavs = rng.sample(wavs, max_files)
        wavs = sorted(wavs)
    print(f"[info] loading {label} pool: {len(wavs)} files from {speech_dir}")
    pool: list[VoiceSource] = []
    for p in tqdm(wavs, desc=f"load-{label}", dynamic_ncols=True):
        wav, _ = librosa.load(str(p), sr=sr, mono=True)
        if wav.size == 0:
            continue
        peak = float(np.max(np.abs(wav)) + 1e-9)
        wav = (wav.astype(np.float32) / peak * 0.6).astype(np.float32)
        pool.append(VoiceSource(path=p, speaker_id=_speaker_id_from_path(p), wav=wav))
    if not pool:
        raise RuntimeError(f"No usable audio under: {speech_dir}")
    return pool


def _write_sample(
    output_dir: Path,
    metadata: dict[str, object],
    mixture: np.ndarray,
    inside_ref_tracks: list[np.ndarray],
    sr: int,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    sf.write(str(output_dir / "mixture.wav"), mixture.T, sr)
    for vidx, ref_wav in enumerate(inside_ref_tracks):
        sf.write(str(output_dir / f"mic00_voice{vidx:02d}.wav"), ref_wav, sr)
    with open(output_dir / "metadata.json", "w", encoding="utf-8") as f:
        json.dump(metadata, f, indent=2)


def _generate_split(
    *,
    split_name: str,
    n_samples: int,
    voice_pool: Sequence[VoiceSource],
    output_root: Path,
    sr: int,
    duration_s: float,
    dis_threshold: float,
    outside_gap: float,
    n_out_min: int,
    n_out_max: int,
    p_n_in0: float,
    p_n_in1: float,
    start_index: int,
    rt60_list: Optional[Sequence[float]] = None,
) -> None:
    n_total_samps = int(sr * duration_s)
    pbar = tqdm(range(n_samples), desc=f"{split_name}", dynamic_ncols=True)
    for idx in pbar:
        out_idx = int(start_index + idx)
        n_in = _sample_inside_count(p_n_in0, p_n_in1)
        n_out = random.randint(n_out_min, n_out_max)
        n_total = n_in + n_out
        chosen = _choose_sources(voice_pool, n_total)

        # Ensure there is enough free radius around the array center to place
        # outside speakers strictly beyond dis_threshold + outside_gap.
        min_required_radius = dis_threshold + outside_gap + 0.05
        room_dim, mic_center, mics = _sample_room_and_mics()
        for _ in range(40):
            r_max_geom = min(
                mic_center[0] - 0.35,
                room_dim[0] - 0.35 - mic_center[0],
                mic_center[1] - 0.35,
                room_dim[1] - 0.35 - mic_center[1],
            )
            if r_max_geom >= min_required_radius:
                break
            room_dim, mic_center, mics = _sample_room_and_mics()

        sources: list[dict[str, object]] = []
        segments: list[np.ndarray] = []
        positions: list[np.ndarray] = []
        for i, src in enumerate(chosen):
            is_inside = i < n_in
            pos, dist = _sample_source_position(
                mic_center,
                room_dim,
                dis_threshold=dis_threshold,
                outside_gap=outside_gap,
                inside=is_inside,
            )
            seg = _make_segment(src.wav, n_total_samps)
            seg *= float(random.uniform(0.35, 0.95))
            segments.append(seg)
            positions.append(pos)
            ang = float(np.degrees(np.arctan2(pos[1] - mic_center[1], pos[0] - mic_center[0])))
            sources.append(
                {
                    "distance": dist,
                    "angle_deg": ang,
                    "speaker_id": src.speaker_id,
                    "pos": pos.tolist(),
                }
            )

        if segments:
            target_rt60 = float(random.choice(list(rt60_list))) if rt60_list else None
            tracks, rt60, absorption, max_order = _render_pyroom(
                segments,
                positions,
                mics,
                room_dim,
                sr,
                n_total_samps,
                target_rt60=target_rt60,
            )
            for i, track in enumerate(tracks):
                peak = float(np.max(np.abs(track)) + 1e-9)
                scale = float(random.uniform(0.5, 0.9)) / peak
                tracks[i] = (track * scale).astype(np.float32)
            mix = np.sum(np.stack(tracks, axis=0), axis=0).astype(np.float32)
        else:
            tracks = []
            mix = np.zeros((6, n_total_samps), dtype=np.float32)
            rt60, absorption, max_order = 0.0, 0.5, 0

        peak = float(np.max(np.abs(mix)) + 1e-9)
        if peak > 0.99:
            g = 0.95 / peak
            mix = (mix * g).astype(np.float32)
            for i in range(len(tracks)):
                tracks[i] = (tracks[i] * g).astype(np.float32)

        inside_ref_tracks = [tracks[i][0].astype(np.float32) for i in range(n_in)]
        metadata = _build_metadata(
            sources=sources,
            mics=mics,
            n_in=n_in,
            n_out=n_out,
            room_dim=room_dim,
            absorption=absorption,
            max_order=max_order,
            rt60=rt60,
        )
        sample_dir = output_root / split_name / f"{out_idx:05d}"
        _write_sample(sample_dir, metadata, mix.astype(np.float32), inside_ref_tracks, sr)


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Generate stopband-focused LibriSpeech synthetic data.")
    p.add_argument("--speech-dir", type=str, default="datasets/_speech_cache")
    p.add_argument(
        "--val-speech-dir",
        type=str,
        default=None,
        help="Optional separate speech root for val (e.g. LibriSpeech/dev-clean).",
    )
    p.add_argument("--output-path", type=str, required=True)
    p.add_argument("--sr", type=int, default=24000)
    p.add_argument("--duration", type=float, default=5.0)
    p.add_argument("--dis-threshold", type=float, default=1.5)
    p.add_argument("--outside-gap", type=float, default=0.08)
    p.add_argument("--n-out-min", type=int, default=1)
    p.add_argument("--n-out-max", type=int, default=2)
    p.add_argument("--n-train", type=int, default=3000)
    p.add_argument("--n-val", type=int, default=400)
    p.add_argument("--n-test", type=int, default=0)
    p.add_argument("--train-start", type=int, default=0)
    p.add_argument("--val-start", type=int, default=0)
    p.add_argument("--test-start", type=int, default=0)
    p.add_argument("--max-files", type=int, default=1500, help="Cap train speech files loaded into RAM.")
    p.add_argument("--val-max-files", type=int, default=300, help="Cap val speech files loaded into RAM.")
    p.add_argument("--p-n-in0", type=float, default=0.5)
    p.add_argument("--p-n-in1", type=float, default=0.25)
    p.add_argument(
        "--rt60-list",
        type=float,
        nargs="+",
        default=[0.2, 0.8, 1.0, 1.6],
        help="Sample one target RT60 per clip via inverse Sabine (empty = legacy random absorption).",
    )
    p.add_argument("--seed", type=int, default=12)
    return p


def main() -> None:
    args = _build_parser().parse_args()
    random.seed(args.seed)
    np.random.seed(args.seed)

    speech_dir = Path(args.speech_dir).resolve()
    val_speech_dir = Path(args.val_speech_dir).resolve() if args.val_speech_dir else speech_dir
    out_root = Path(args.output_path).resolve()
    out_root.mkdir(parents=True, exist_ok=True)

    train_pool = _build_voice_pool(
        speech_dir,
        int(args.sr),
        max_files=int(args.max_files) if args.max_files else None,
        seed=int(args.seed),
        label="train",
    )
    if args.n_val > 0:
        val_pool = _build_voice_pool(
            val_speech_dir,
            int(args.sr),
            max_files=int(args.val_max_files) if args.val_max_files else None,
            seed=int(args.seed) + 1,
            label="val",
        )
    else:
        val_pool = train_pool

    print(f"[info] train_speech={speech_dir} files={len(train_pool)}")
    print(f"[info] val_speech={val_speech_dir} files={len(val_pool)}")
    print(
        "[info] inside_mix: "
        f"n_in=0 -> {args.p_n_in0:.2f}, n_in=1 -> {args.p_n_in1:.2f}, "
        f"n_in=2 -> {1.0 - args.p_n_in0 - args.p_n_in1:.2f}"
    )
    rt60_list = [float(x) for x in (args.rt60_list or [])]
    print(f"[info] dis_threshold={args.dis_threshold}, outside_gap={args.outside_gap}")
    print(f"[info] rt60_list={rt60_list if rt60_list else 'legacy-random-absorption'}")

    split_kwargs = dict(
        output_root=out_root,
        sr=int(args.sr),
        duration_s=float(args.duration),
        dis_threshold=float(args.dis_threshold),
        outside_gap=float(args.outside_gap),
        n_out_min=int(args.n_out_min),
        n_out_max=int(args.n_out_max),
        p_n_in0=float(args.p_n_in0),
        p_n_in1=float(args.p_n_in1),
        rt60_list=rt60_list if rt60_list else None,
    )

    if args.n_train > 0:
        _generate_split(
            split_name="train",
            n_samples=int(args.n_train),
            voice_pool=train_pool,
            start_index=int(args.train_start),
            **split_kwargs,
        )
    if args.n_val > 0:
        _generate_split(
            split_name="val",
            n_samples=int(args.n_val),
            voice_pool=val_pool,
            start_index=int(args.val_start),
            **split_kwargs,
        )
    if args.n_test > 0:
        _generate_split(
            split_name="test",
            n_samples=int(args.n_test),
            voice_pool=train_pool,
            start_index=int(args.test_start),
            **split_kwargs,
        )

    args_out = vars(args).copy()
    args_out["speech_dir"] = str(speech_dir)
    args_out["val_speech_dir"] = str(val_speech_dir)
    args_out["output_path"] = str(out_root)
    with open(out_root / "args.json", "w", encoding="utf-8") as f:
        json.dump(args_out, f, indent=2)
    print(f"[ok] wrote dataset: {out_root}")


if __name__ == "__main__":
    main()
