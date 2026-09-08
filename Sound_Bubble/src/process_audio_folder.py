import argparse
import os
import sys
from pathlib import Path

import librosa
import numpy as np
import torch

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

import src.utils as utils
from src.utils import write_audio_file


SUPPORTED_EXTS = {".wav", ".mp3", ".flac", ".m4a", ".ogg"}


def dbfs_to_linear(dbfs: float) -> float:
    return float(10.0 ** (dbfs / 20.0))


def linear_to_dbfs(value: float, floor: float = 1e-12) -> float:
    return float(20.0 * np.log10(max(float(value), floor)))


def get_distance_embedding(distance_threshold: float, device: torch.device) -> torch.Tensor:
    mapping = {
        1.0: [0.0, 0.0, 1.0],
        1.5: [0.0, 1.0, 0.0],
        2.0: [1.0, 0.0, 0.0],
    }
    if distance_threshold not in mapping:
        raise ValueError("distance_threshold must be one of {1.0, 1.5, 2.0}")
    return torch.tensor([mapping[distance_threshold]], dtype=torch.float32, device=device)


def fit_channels(audio: np.ndarray, target_ch: int) -> np.ndarray:
    # audio: [C, T]
    if audio.ndim == 1:
        audio = audio[None, :]

    c = audio.shape[0]
    if c == target_ch:
        return audio
    if c > target_ch:
        return audio[:target_ch]

    # Block-repeat each channel (e.g. 2 -> 6 becomes L,L,L,R,R,R).
    base = target_ch // c
    rem = target_ch % c
    parts = []
    for i in range(c):
        nrep = base + (1 if i < rem else 0)
        if nrep > 0:
            parts.append(np.tile(audio[i : i + 1], (nrep, 1)))
    return np.concatenate(parts, axis=0)


def resample_channels(audio: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    if sr_in == sr_out:
        return audio.astype(np.float32)
    if audio.ndim == 1:
        return librosa.resample(audio.astype(np.float32), orig_sr=sr_in, target_sr=sr_out).astype(np.float32)
    return np.stack(
        [
            librosa.resample(ch.astype(np.float32), orig_sr=sr_in, target_sr=sr_out).astype(np.float32)
            for ch in audio
        ],
        axis=0,
    )


def suppress_full_scale_glitches(
    audio: np.ndarray,
    sr: int,
    threshold_dbfs: float,
    mute_ms: float,
):
    """Mute short windows around near-full-scale impulses to avoid loud pops."""
    if audio.ndim == 1:
        audio = audio[None, :]

    cleaned = audio.astype(np.float32, copy=True)
    threshold = dbfs_to_linear(threshold_dbfs)
    mute_radius = max(0, int(round(sr * mute_ms / 1000.0)))

    raw_glitch_counts = np.zeros(cleaned.shape[0], dtype=np.int32)
    muted_sample_counts = np.zeros(cleaned.shape[0], dtype=np.int32)

    for ch in range(cleaned.shape[0]):
        hot_idx = np.flatnonzero(np.abs(cleaned[ch]) >= threshold)
        raw_glitch_counts[ch] = int(hot_idx.size)
        if hot_idx.size == 0:
            continue

        expanded = np.zeros(cleaned.shape[-1] + 1, dtype=np.int32)
        starts = np.maximum(hot_idx - mute_radius, 0)
        ends = np.minimum(hot_idx + mute_radius + 1, cleaned.shape[-1])
        np.add.at(expanded, starts, 1)
        np.add.at(expanded, ends, -1)
        mute_mask = np.cumsum(expanded[:-1]) > 0

        cleaned[ch, mute_mask] = 0.0
        muted_sample_counts[ch] = int(mute_mask.sum())

    return cleaned, raw_glitch_counts, muted_sample_counts


def robust_global_normalize(
    audio: np.ndarray,
    target_peak_dbfs: float,
    max_gain_db: float,
    activity_floor_dbfs: float,
    level_percentile: float,
):
    """Apply one shared gain across active channels to preserve spatial cues."""
    if audio.ndim == 1:
        audio = audio[None, :]

    cleaned = audio.astype(np.float32, copy=False)
    robust_levels = np.zeros(cleaned.shape[0], dtype=np.float32)

    for ch in range(cleaned.shape[0]):
        mag = np.abs(cleaned[ch])
        mag = mag[mag > 0.0]
        if mag.size == 0:
            robust_levels[ch] = 0.0
            continue
        robust_levels[ch] = float(np.percentile(mag, level_percentile))

    robust_levels_dbfs = np.array(
        [linear_to_dbfs(level) if level > 0.0 else -120.0 for level in robust_levels],
        dtype=np.float32,
    )
    active_mask = robust_levels_dbfs > activity_floor_dbfs

    gain_db = 0.0
    if np.any(active_mask):
        ref_level = float(np.max(robust_levels[active_mask]))
        target_level = dbfs_to_linear(target_peak_dbfs)
        if ref_level > 0.0:
            gain_db = min(max_gain_db, linear_to_dbfs(target_level / ref_level))

    gain_linear = dbfs_to_linear(gain_db)
    normalized = np.clip(cleaned * gain_linear, -1.0, 1.0)
    return normalized, gain_db, robust_levels_dbfs, active_mask


def preprocess_model_input(
    audio: np.ndarray,
    sr: int,
    glitch_threshold_dbfs: float,
    glitch_mute_ms: float,
    target_peak_dbfs: float,
    max_gain_db: float,
    activity_floor_dbfs: float,
    level_percentile: float,
):
    glitch_suppressed, raw_glitches, muted_samples = suppress_full_scale_glitches(
        audio=audio,
        sr=sr,
        threshold_dbfs=glitch_threshold_dbfs,
        mute_ms=glitch_mute_ms,
    )
    normalized, gain_db, robust_levels_dbfs, active_mask = robust_global_normalize(
        glitch_suppressed,
        target_peak_dbfs=target_peak_dbfs,
        max_gain_db=max_gain_db,
        activity_floor_dbfs=activity_floor_dbfs,
        level_percentile=level_percentile,
    )
    stats = {
        "gain_db": float(gain_db),
        "raw_glitches": raw_glitches.tolist(),
        "muted_samples": muted_samples.tolist(),
        "robust_levels_dbfs": [float(v) for v in robust_levels_dbfs],
        "active_channels": [int(i) for i in np.flatnonzero(active_mask)],
    }
    return normalized, stats


def choose_loud_noisy_window(audio: np.ndarray, sr: int, window_sec: float, hop_sec: float = 1.0):
    """Pick a window that is both loud and noise-rich.

    Heuristic score combines:
    - loudness: RMS energy
    - noisiness: RMS of first-order difference
    """
    if audio.ndim == 1:
        ref = audio.astype(np.float32)
    else:
        ref = audio[0].astype(np.float32)

    n = ref.shape[0]
    win = max(1, int(window_sec * sr))
    hop = max(1, int(hop_sec * sr))

    if n <= win:
        return 0, n

    best_score = -1.0
    best_start = 0

    # Slide coarse windows for fast demo selection.
    for start in range(0, n - win + 1, hop):
        seg = ref[start : start + win]
        rms = float(np.sqrt(np.mean(seg * seg) + 1e-12))
        d = np.diff(seg, prepend=seg[:1])
        noisy = float(np.sqrt(np.mean(d * d) + 1e-12))
        score = rms * noisy
        if score > best_score:
            best_score = score
            best_start = start

    return best_start, min(best_start + win, n)


def slice_demo_window(
    audio: np.ndarray,
    sr: int,
    demo_seconds: float,
    window_mode: str,
    window_start_sec: float,
):
    n = audio.shape[-1]
    win = max(1, int(demo_seconds * sr))
    if n <= win:
        return audio, 0, n

    if window_mode == "manual":
        start = int(max(0.0, window_start_sec) * sr)
        start = min(start, max(0, n - win))
        end = start + win
    elif window_mode == "start":
        start = 0
        end = min(win, n)
    else:
        start, end = choose_loud_noisy_window(audio, sr, demo_seconds, hop_sec=1.0)

    return audio[..., start:end], start, end


def process_array(
    model,
    audio: np.ndarray,
    sr: int,
    model_num_ch: int,
    model_chunk: int,
    model_pad: int,
    dis_embed: torch.Tensor,
    device: torch.device,
    verbose: bool = False
):
    audio = fit_channels(audio, model_num_ch).astype(np.float32)
    state = model.init_buffers(batch_size=1, device=device)
    total_len = audio.shape[-1]

    frame = torch.zeros(
        (1, model_num_ch, model_chunk + model_pad),
        dtype=torch.float32,
        device=device,
    )

    est_chunks = []
    # Chunked streaming inference avoids full-file VRAM spikes.
    num_chunks = (total_len + model_chunk - 1) // model_chunk
    for chunk_idx, start in enumerate(range(0, total_len, model_chunk), start=1):
        end = min(start + model_chunk, total_len)
        chunk_np = audio[:, start:end]
        if chunk_np.shape[-1] < model_chunk:
            pad_width = model_chunk - chunk_np.shape[-1]
            chunk_np = np.pad(chunk_np, ((0, 0), (0, pad_width)), mode="constant")

        chunk = torch.from_numpy(chunk_np).to(device)
        frame = torch.roll(frame, shifts=-model_chunk, dims=-1)
        frame[0, :, -model_chunk:] = chunk

        with torch.no_grad():
            if device.type == "cuda":
                with torch.amp.autocast("cuda", dtype=torch.float16):
                    outputs = model(
                        {"mixture": frame, "dis_embed": dis_embed},
                        input_state=state,
                        pad=False,
                    )
            else:
                outputs = model(
                    {"mixture": frame, "dis_embed": dis_embed},
                    input_state=state,
                    pad=False,
                )
            state = outputs["next_state"]
            est_chunk = outputs["output"][0, :, :model_chunk].detach().float().cpu().numpy()
        est_chunks.append(est_chunk)
        if verbose and chunk_idx % 200 == 0:
            print(f"  processed chunks: {chunk_idx}/{num_chunks}", flush=True)

    est = np.concatenate(est_chunks, axis=-1)[:, :total_len]

    return est


def process_one_file(
    model,
    path: Path,
    out_path: Path,
    sr: int,
    model_num_ch: int,
    model_chunk: int,
    model_pad: int,
    dis_embed: torch.Tensor,
    device: torch.device,
    verbose: bool = False,
):
    audio, _ = librosa.load(str(path), mono=False, sr=sr)
    est = process_array(
        model=model,
        audio=audio,
        sr=sr,
        model_num_ch=model_num_ch,
        model_chunk=model_chunk,
        model_pad=model_pad,
        dis_embed=dis_embed,
        device=device,
        verbose=verbose,
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    write_audio_file(str(out_path), est, sr=sr)


def main(args):
    device = torch.device("cuda" if args.use_cuda and torch.cuda.is_available() else "cpu")
    if args.use_cuda and device.type != "cuda":
        print("WARNING: CUDA requested but unavailable. Falling back to CPU.")

    pl_module, params = utils.load_torch_pretrained(args.run_dir, return_params=True, map_location="cpu")
    model = pl_module.model.to(device).eval()

    model_params = params["pl_module_args"]["model_params"]
    model_num_ch = int(model_params["num_ch"])
    model_chunk = int(model_params["stft_chunk_size"])
    model_pad = int(model_params["stft_pad_size"])
    model_sr = int(params["pl_module_args"].get("sr", 24000))
    work_sr = model_sr
    dis_embed = get_distance_embedding(args.distance_threshold, device)

    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    files = [
        p for p in sorted(input_dir.iterdir())
        if p.is_file()
        and p.suffix.lower() in SUPPORTED_EXTS
        and p.resolve() != output_dir.resolve()
        and not p.stem.endswith(("_est", "_demo_est", "_demo_in"))
    ]
    if args.max_files is not None:
        files = files[: max(0, args.max_files)]
    if not files:
        raise FileNotFoundError(f"No audio files found in {input_dir}")

    print(
        f"Found {len(files)} file(s). device={device}, model_sr={model_sr}, "
        f"model_num_ch={model_num_ch}, chunk={model_chunk}, pad={model_pad}"
    , flush=True)

    for idx, path in enumerate(files, start=1):
        out_name = f"{path.stem}_est.wav"
        out_path = output_dir / out_name
        print(f"[{idx}/{len(files)}] {path.name} -> {out_path.name}", flush=True)
        raw_audio, file_sr = librosa.load(str(path), mono=False, sr=None)
        input_sr = file_sr
        if args.sr is not None:
            input_sr = args.sr
            raw_audio = resample_channels(raw_audio, sr_in=file_sr, sr_out=input_sr)
        if not args.disable_input_preprocess:
            raw_audio, prep_stats = preprocess_model_input(
                audio=raw_audio,
                sr=input_sr,
                glitch_threshold_dbfs=args.glitch_threshold_dbfs,
                glitch_mute_ms=args.glitch_mute_ms,
                target_peak_dbfs=args.target_input_peak_dbfs,
                max_gain_db=args.max_input_gain_db,
                activity_floor_dbfs=args.activity_floor_dbfs,
                level_percentile=args.level_percentile,
            )
            print(
                "  preprocess: "
                f"active={prep_stats['active_channels']} "
                f"gain={prep_stats['gain_db']:.1f}dB "
                f"robust_dbfs={[round(v, 1) for v in prep_stats['robust_levels_dbfs']]} "
                f"glitches={prep_stats['raw_glitches']} "
                f"muted={prep_stats['muted_samples']}",
                flush=True,
            )
        save_sr = input_sr if args.save_sr == "input" else model_sr
        raw_audio_work = resample_channels(raw_audio, sr_in=input_sr, sr_out=work_sr)
        model_audio = fit_channels(raw_audio_work, model_num_ch).astype(np.float32)

        if args.demo_seconds is not None:
            model_audio_slice, s0, s1 = slice_demo_window(
                model_audio,
                sr=work_sr,
                demo_seconds=args.demo_seconds,
                window_mode=args.window_mode,
                window_start_sec=args.window_start_sec,
            )
            raw_audio_slice_work = raw_audio_work[..., s0:s1] if raw_audio_work.ndim > 1 else raw_audio_work[s0:s1]
            raw_audio_slice = resample_channels(raw_audio_slice_work, sr_in=work_sr, sr_out=save_sr)
            print(
                f"  demo window: {s0/work_sr:.2f}s -> {s1/work_sr:.2f}s "
                f"(len={model_audio_slice.shape[-1]/work_sr:.2f}s), save_sr={save_sr}",
                flush=True,
            )
            demo_in = output_dir / f"{path.stem}_demo_in.wav"
            write_audio_file(str(demo_in), raw_audio_slice, sr=save_sr)
            out_path = output_dir / f"{path.stem}_demo_est.wav"
            est = process_array(
                model=model,
                audio=model_audio_slice,
                sr=work_sr,
                model_num_ch=model_num_ch,
                model_chunk=model_chunk,
                model_pad=model_pad,
                dis_embed=dis_embed,
                device=device,
                verbose=args.verbose,
            )
            est_save = resample_channels(est, sr_in=work_sr, sr_out=save_sr)
            write_audio_file(str(out_path), est_save, sr=save_sr)
        else:
            est = process_array(
                model=model,
                audio=model_audio,
                sr=work_sr,
                model_num_ch=model_num_ch,
                model_chunk=model_chunk,
                model_pad=model_pad,
                dis_embed=dis_embed,
                device=device,
                verbose=args.verbose,
            )
            est_save = resample_channels(est, sr_in=work_sr, sr_out=save_sr)
            write_audio_file(str(out_path), est_save, sr=save_sr)
        print(f"  done: {out_path}", flush=True)

    print(f"Done. Enhanced files saved to: {output_dir}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Batch process an audio folder with Sound_Bubble model.")
    parser.add_argument("input_dir", type=str, help="Folder containing input audio files.")
    parser.add_argument("run_dir", type=str, help="Model run dir with config.json and checkpoints/best.pt.")
    parser.add_argument("output_dir", type=str, help="Folder to write processed WAV files.")
    parser.add_argument(
        "--distance_threshold",
        type=float,
        default=1.0,
        help="Distance embedding value. One of {1.0, 1.5, 2.0}.",
    )
    parser.add_argument("--sr", type=int, default=None, help="Optional input resample rate before processing.")
    parser.add_argument(
        "--disable_input_preprocess",
        action="store_true",
        help="Disable the default glitch suppression + robust input normalization stage.",
    )
    parser.add_argument(
        "--glitch_threshold_dbfs",
        type=float,
        default=-2.0,
        help="Mute windows around samples above this near-full-scale threshold.",
    )
    parser.add_argument(
        "--glitch_mute_ms",
        type=float,
        default=2.0,
        help="Mute this many milliseconds on each side of a detected full-scale glitch.",
    )
    parser.add_argument(
        "--target_input_peak_dbfs",
        type=float,
        default=-12.0,
        help="Shared robust target level for active channels before model inference.",
    )
    parser.add_argument(
        "--max_input_gain_db",
        type=float,
        default=24.0,
        help="Cap for the shared normalization gain applied before inference.",
    )
    parser.add_argument(
        "--activity_floor_dbfs",
        type=float,
        default=-70.0,
        help="Channels below this robust level are treated as inactive during normalization.",
    )
    parser.add_argument(
        "--level_percentile",
        type=float,
        default=95.0,
        help="Percentile used to estimate robust active-channel level for normalization.",
    )
    parser.add_argument(
        "--save_sr",
        type=str,
        default="input",
        choices=["input", "model"],
        help="Output save sample rate: original input rate or model rate.",
    )
    parser.add_argument("--use_cuda", action="store_true", help="Use CUDA if available.")
    parser.add_argument("--max_files", type=int, default=None, help="Optionally limit number of files to process.")
    parser.add_argument("--verbose", action="store_true", help="Print periodic chunk progress.")
    parser.add_argument(
        "--demo_seconds",
        type=float,
        default=None,
        help="If set, only process this many seconds per file (demo mode).",
    )
    parser.add_argument(
        "--window_mode",
        type=str,
        default="loudest_noisiest",
        choices=["loudest_noisiest", "start", "manual"],
        help="How to choose demo window when --demo_seconds is set.",
    )
    parser.add_argument(
        "--window_start_sec",
        type=float,
        default=0.0,
        help="Manual window start time in seconds (used when --window_mode manual).",
    )
    main(parser.parse_args())
