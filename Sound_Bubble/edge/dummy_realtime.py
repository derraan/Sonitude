from __future__ import annotations

import argparse
import math
import os
import queue
import sys
import threading
import time
import traceback

import numpy as np
from scipy.signal import resample_poly
try:
    import soxr
except ImportError:
    soxr = None

try:
    import sounddevice as sd
except ImportError:
    sd = None

import torch

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

import src.utils as utils

from edge.realtime.ring_buffer import OutputRingBuffer
from edge.realtime.dry_reference import DryReferenceRing
from edge.pipeline.processing import StreamingResampler


def list_audio_devices() -> None:
    if sd is None:
        raise RuntimeError(
            "sounddevice is not installed. Please install it: pip install sounddevice"
        )
    devices = sd.query_devices()
    hostapis = sd.query_hostapis()
    print("Audio devices:\n")
    for index, device in enumerate(devices):
        hostapi_name = hostapis[device["hostapi"]]["name"]
        print(
            f"[{index}] {device['name']} | "
            f"hostapi={hostapi_name} | "
            f"in={device['max_input_channels']} "
            f"out={device['max_output_channels']} "
            f"default_sr={device['default_samplerate']}"
        )


def parse_channel_map(channel_map_arg: str, input_channels: int, model_num_ch: int):
    if channel_map_arg is None:
        return list(range(min(input_channels, model_num_ch)))

    channel_map = []
    for token in channel_map_arg.split(","):
        token = token.strip()
        if not token:
            continue
        try:
            idx = int(token)
        except ValueError as exc:
            raise ValueError(
                f"Invalid --channel-map value {channel_map_arg!r}. Use comma-separated integers, e.g. 0,1,2,3,4,5."
            ) from exc
        if idx < 0 or idx >= input_channels:
            raise ValueError(
                f"Channel index {idx} in --channel-map is out of range for input_channels={input_channels}."
            )
        channel_map.append(idx)

    if not channel_map:
        raise ValueError("--channel-map did not contain any valid channel indices.")
    if len(channel_map) > model_num_ch:
        raise ValueError(
            f"--channel-map selects {len(channel_map)} channels, but the model expects at most {model_num_ch}."
        )
    return channel_map


def parse_channel_list(channel_list_arg: str, input_channels: int):
    if channel_list_arg is None:
        return []

    channel_list = []
    for token in channel_list_arg.split(","):
        token = token.strip()
        if not token:
            continue
        try:
            idx = int(token)
        except ValueError as exc:
            raise ValueError(
                f"Invalid channel list value {channel_list_arg!r}. Use comma-separated integers, e.g. 0,1."
            ) from exc
        if idx < 0 or idx >= input_channels:
            raise ValueError(f"Channel index {idx} is out of range for input_channels={input_channels}.")
        channel_list.append(idx)

    return sorted(set(channel_list))


def _rms_dbfs(x: np.ndarray) -> float:
    rms = float(np.sqrt(np.mean(np.square(x), dtype=np.float64) + 1e-12))
    return 20.0 * np.log10(max(rms, 1e-8))


def _dbfs_to_linear(dbfs: float) -> float:
    return float(10.0 ** (dbfs / 20.0))


def _linear_to_dbfs(value: float, floor: float = 1e-12) -> float:
    return float(20.0 * np.log10(max(float(value), floor)))


def _smooth_db(prev: np.ndarray, cur: np.ndarray, alpha: float = 0.8) -> np.ndarray:
    if prev.shape != cur.shape:
        return cur
    return alpha * prev + (1.0 - alpha) * cur


def _format_db_list(prefix: str, values: np.ndarray) -> str:
    parts = []
    for idx, db in enumerate(values):
        parts.append(f"{idx}:{db:5.1f}")
    return f"{prefix}[{', '.join(parts)}]"


def _suppress_full_scale_glitches(
    audio: np.ndarray,
    sr: int,
    threshold_dbfs: float,
    mute_ms: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Mute short windows around near-full-scale spikes in [C, T] audio."""
    cleaned = audio.astype(np.float32, copy=True)
    threshold = _dbfs_to_linear(threshold_dbfs)
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


def _robust_global_normalize(
    audio: np.ndarray,
    target_peak_dbfs: float,
    max_gain_db: float,
    activity_floor_dbfs: float,
    level_percentile: float,
) -> tuple[np.ndarray, float, np.ndarray, np.ndarray]:
    """Apply one shared gain across active channels to preserve spatial cues."""
    robust_levels = np.zeros(audio.shape[0], dtype=np.float32)

    for ch in range(audio.shape[0]):
        mag = np.abs(audio[ch])
        mag = mag[mag > 0.0]
        if mag.size == 0:
            continue
        robust_levels[ch] = float(np.percentile(mag, level_percentile))

    robust_levels_dbfs = np.array(
        [_linear_to_dbfs(level) if level > 0.0 else -120.0 for level in robust_levels],
        dtype=np.float32,
    )
    active_mask = robust_levels_dbfs > activity_floor_dbfs

    gain_db = 0.0
    if np.any(active_mask):
        ref_level = float(np.max(robust_levels[active_mask]))
        target_level = _dbfs_to_linear(target_peak_dbfs)
        if ref_level > 0.0:
            gain_db = min(max_gain_db, _linear_to_dbfs(target_level / ref_level))

    gain_linear = _dbfs_to_linear(gain_db)
    normalized = np.clip(audio * gain_linear, -1.0, 1.0)
    return normalized, gain_db, robust_levels_dbfs, active_mask


def _preprocess_live_input(
    audio: np.ndarray,
    sr: int,
    threshold_dbfs: float,
    mute_ms: float,
    target_peak_dbfs: float,
    max_gain_db: float,
    activity_floor_dbfs: float,
    level_percentile: float,
) -> tuple[np.ndarray, dict]:
    glitch_suppressed, raw_glitches, muted_samples = _suppress_full_scale_glitches(
        audio=audio,
        sr=sr,
        threshold_dbfs=threshold_dbfs,
        mute_ms=mute_ms,
    )
    normalized, gain_db, robust_levels_dbfs, active_mask = _robust_global_normalize(
        audio=glitch_suppressed,
        target_peak_dbfs=target_peak_dbfs,
        max_gain_db=max_gain_db,
        activity_floor_dbfs=activity_floor_dbfs,
        level_percentile=level_percentile,
    )
    stats = {
        "gain_db": float(gain_db),
        "raw_glitches": raw_glitches,
        "muted_samples": muted_samples,
        "robust_levels_dbfs": robust_levels_dbfs,
        "active_mask": active_mask,
    }
    return normalized, stats


def _simple_input_normalize(
    audio: np.ndarray,
    target_rms_dbfs: float,
    max_gain_db: float,
) -> tuple[np.ndarray, float]:
    """Apply one global gain so mixed-channel RMS approaches target_rms_dbfs."""
    x = audio.astype(np.float32, copy=False)
    cur_rms = float(np.sqrt(np.mean(np.square(x), dtype=np.float64) + 1e-12))
    target_rms = _dbfs_to_linear(target_rms_dbfs)
    if cur_rms <= 1e-12:
        return x, 0.0
    gain_db = _linear_to_dbfs(target_rms / cur_rms)
    gain_db = float(np.clip(gain_db, -max_gain_db, max_gain_db))
    gain_lin = _dbfs_to_linear(gain_db)
    y = np.clip(x * gain_lin, -1.0, 1.0)
    return y, gain_db


def _apply_fixed_input_gain(audio: np.ndarray, gain_db: float) -> np.ndarray:
    x = audio.astype(np.float32, copy=False)
    gain_lin = _dbfs_to_linear(float(gain_db))
    return np.clip(x * gain_lin, -1.0, 1.0)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run Sound_Bubble model in a dummy real-time loop."
    )
    parser.add_argument(
        "run_dir",
        nargs="?",
        type=str,
        default=None,
        help="Path to trained run directory (contains config.json and checkpoints/best.pt).",
    )
    parser.add_argument(
        "--config-path",
        type=str,
        default=None,
        help="Path to experiment config JSON (alternative to run_dir).",
    )
    parser.add_argument(
        "--checkpoint-path",
        type=str,
        default=None,
        help="Optional checkpoint path used with --config-path.",
    )
    parser.add_argument(
        "--distance-threshold",
        type=float,
        default=1.0,
        choices=[1.0, 1.5, 2.0],
        help="Bubble size embedding used by the model.",
    )
    parser.add_argument(
        "--input-device",
        type=int,
        default=None,
        help="Sounddevice input device index.",
    )
    parser.add_argument(
        "--output-device",
        type=int,
        default=None,
        help="Sounddevice output device index.",
    )
    parser.add_argument(
        "--io-sr",
        type=int,
        default=None,
        help="Legacy shared I/O sample rate. If set, it is used for both input and output unless overridden.",
    )
    parser.add_argument(
        "--input-sr",
        type=int,
        default=None,
        help="Input device sample rate. Defaults to --io-sr or 16000.",
    )
    parser.add_argument(
        "--output-sr",
        type=int,
        default=None,
        help="Output device sample rate. Defaults to --io-sr or the input sample rate.",
    )
    parser.add_argument(
        "--input-channels",
        type=int,
        default=2,
        help="Number of channels to read from input device.",
    )
    parser.add_argument(
        "--output-channels",
        type=int,
        default=2,
        help="Number of channels to write to output device.",
    )
    parser.add_argument(
        "--model-device",
        type=str,
        default="cpu",
        choices=["cpu", "cuda"],
        help="Torch device for model execution.",
    )
    parser.add_argument(
        "--latency",
        type=str,
        default="low",
        choices=["low", "high"],
        help="Sounddevice stream latency preset.",
    )
    parser.add_argument("--require-soxr", action="store_true", help="Fail if streaming SoXR backend is unavailable.")
    parser.add_argument(
        "--list-devices",
        "--list_devices",
        action="store_true",
        help="List sounddevice devices and exit.",
    )
    parser.add_argument(
        "--passthrough",
        action="store_true",
        help="Bypass model and route input directly to output.",
    )
    parser.add_argument(
        "--output-gain",
        type=float,
        default=1.0,
        help="Linear gain applied to output signal.",
    )
    parser.add_argument(
        "--mix-dry",
        type=float,
        default=0.0,
        help="Dry/wet mix in [0,1]. 0=model only, 1=input only.",
    )
    parser.add_argument(
        "--print-levels",
        action="store_true",
        help="Print periodic realtime input/output levels and queue stats.",
    )
    parser.add_argument(
        "--channel-map",
        type=str,
        default=None,
        help="Comma-separated input channel indices to feed the model, e.g. 0,1,2,3,4,5.",
    )
    parser.add_argument(
        "--monitor-input-channel",
        type=int,
        default=None,
        help="Input channel index used for passthrough/dry mix monitoring. Defaults to the first mapped channel.",
    )
    parser.add_argument(
        "--mute-input-channels",
        type=str,
        default=None,
        help="Comma-separated input channel indices to zero out before monitoring/processing, e.g. 0 or 0,6,7.",
    )
    parser.add_argument(
        "--level-interval",
        type=float,
        default=0.25,
        help="Seconds between realtime level reports when --print-levels is enabled.",
    )
    parser.add_argument(
        "--disable-input-preprocess",
        action="store_true",
        help="Disable near-full-scale glitch suppression and shared input normalization.",
    )
    parser.add_argument(
        "--glitch-threshold-dbfs",
        type=float,
        default=-2.0,
        help="Mute short windows around samples above this near-full-scale threshold.",
    )
    parser.add_argument(
        "--glitch-mute-ms",
        type=float,
        default=2.0,
        help="Mute this many milliseconds on each side of a detected full-scale glitch.",
    )
    parser.add_argument(
        "--target-input-peak-dbfs",
        type=float,
        default=-12.0,
        help="Shared robust target level for active channels before model inference.",
    )
    parser.add_argument(
        "--max-input-gain-db",
        type=float,
        default=24.0,
        help="Cap for the shared normalization gain applied before inference.",
    )
    parser.add_argument(
        "--activity-floor-dbfs",
        type=float,
        default=-70.0,
        help="Channels below this robust level are treated as inactive during normalization.",
    )
    parser.add_argument(
        "--level-percentile",
        type=float,
        default=95.0,
        help="Percentile used to estimate robust active-channel level for normalization.",
    )
    parser.add_argument(
        "--amp",
        action="store_true",
        help="Use mixed precision autocast on CUDA for faster inference.",
    )
    parser.add_argument(
        "--simple-input-normalize",
        action="store_true",
        help="Use simple global RMS normalization instead of glitch/robust preprocessing.",
    )
    parser.add_argument(
        "--simple-target-rms-dbfs",
        type=float,
        default=-20.0,
        help="Target RMS level used by --simple-input-normalize.",
    )
    parser.add_argument(
        "--fixed-input-gain-db",
        type=float,
        default=None,
        help="Apply a constant input gain in dB as an additional preprocessing stage.",
    )
    return parser.parse_args()


def get_distance_embedding(distance_threshold: float, device: torch.device) -> torch.Tensor:
    mapping = {
        1.0: [0.0, 0.0, 1.0],
        1.5: [0.0, 1.0, 0.0],
        2.0: [1.0, 0.0, 0.0],
    }
    return torch.tensor([mapping[distance_threshold]], dtype=torch.float32, device=device)


def _fit_channels(x: np.ndarray, target_channels: int) -> np.ndarray:
    # x: [C, T]
    c, t = x.shape
    if c == target_channels:
        return x
    if c > target_channels:
        return x[:target_channels]
    repeat = target_channels - c
    reps = int(np.ceil(repeat / c))
    cycled = np.tile(x, (reps + 1, 1))
    padded = np.concatenate([x, cycled[:repeat]], axis=0)
    return padded


def _to_stereo_or_multich(y: np.ndarray, out_channels: int) -> np.ndarray:
    # y: [T], returns [T, out_channels]
    y = y.astype(np.float32)
    if out_channels == 1:
        return y[:, None]
    return np.tile(y[:, None], (1, out_channels))


def _resample_poly_exact(x: np.ndarray, src_sr: int, dst_sr: int, axis: int = -1) -> np.ndarray:
    """High-quality polyphase SRC with exact output length."""
    x = np.asarray(x)
    if src_sr <= 0 or dst_sr <= 0:
        raise ValueError("Sample rates must be positive.")
    if x.shape[axis] == 0 or src_sr == dst_sr:
        return x.astype(np.float32, copy=False)

    g = math.gcd(src_sr, dst_sr)
    up = dst_sr // g
    down = src_sr // g

    y = resample_poly(x, up=up, down=down, axis=axis, window=("kaiser", 8.6))

    in_len = x.shape[axis]
    out_len = int(round(in_len * dst_sr / src_sr))
    if y.shape[axis] > out_len:
        slicer = [slice(None)] * y.ndim
        slicer[axis] = slice(0, out_len)
        y = y[tuple(slicer)]
    elif y.shape[axis] < out_len:
        pad_width = [(0, 0)] * y.ndim
        pad_width[axis] = (0, out_len - y.shape[axis])
        y = np.pad(y, pad_width, mode="constant")

    return y.astype(np.float32, copy=False)


def _resample_exact(x: np.ndarray, src_sr: int, dst_sr: int, axis: int = -1) -> np.ndarray:
    """Prefer SoXR VHQ when available; fall back to scipy polyphase."""
    x = np.asarray(x)
    if src_sr <= 0 or dst_sr <= 0:
        raise ValueError("Sample rates must be positive.")
    if x.shape[axis] == 0 or src_sr == dst_sr:
        return x.astype(np.float32, copy=False)

    if soxr is None:
        return _resample_poly_exact(x, src_sr=src_sr, dst_sr=dst_sr, axis=axis)

    axis = axis if axis >= 0 else x.ndim + axis
    moved = np.moveaxis(x, axis, -1)
    flat = moved.reshape(-1, moved.shape[-1])
    out_len = int(round(moved.shape[-1] * dst_sr / src_sr))
    out = np.empty((flat.shape[0], out_len), dtype=np.float32)
    for i in range(flat.shape[0]):
        out[i] = soxr.resample(flat[i], in_rate=src_sr, out_rate=dst_sr, quality="VHQ")
    out = out.reshape(*moved.shape[:-1], out_len)
    out = np.moveaxis(out, -1, axis)
    return out.astype(np.float32, copy=False)


def main():
    args = parse_args()

    if args.list_devices:
        list_audio_devices()
        return

    if sd is None:
        raise RuntimeError(
            "sounddevice is not installed. Please install it in your environment: pip install sounddevice"
        )

    requested_device = torch.device(args.model_device)
    if requested_device.type == "cuda" and not torch.cuda.is_available():
        print("WARNING: CUDA requested but unavailable. Falling back to CPU.")
        requested_device = torch.device("cpu")

    if args.config_path is not None:
        pl_module, params = utils.load_net_torch(args.config_path, return_params=True)
        if args.checkpoint_path:
            print(f"Loading checkpoint from {args.checkpoint_path}")
            pl_module.load_state(args.checkpoint_path, map_location="cpu")
            print(f"Loaded module at epoch {pl_module.epoch}")
        else:
            print("WARNING: No checkpoint provided. Running with random weights.")
    else:
        if args.run_dir is None:
            raise ValueError("Provide either run_dir or --config-path.")
        pl_module, params = utils.load_torch_pretrained(args.run_dir, return_params=True, map_location="cpu")

    model = pl_module.model.to(requested_device)
    model.eval()

    model_params = params["pl_module_args"]["model_params"]
    model_sr = int(params["pl_module_args"].get("sr", 24000))
    model_chunk = int(model_params["stft_chunk_size"])
    model_pad = int(model_params["stft_pad_size"])
    model_num_ch = int(model_params["num_ch"])
    input_sr = int(args.input_sr or args.io_sr or 16000)
    output_sr = int(args.output_sr or args.io_sr or input_sr)
    selected_channels = parse_channel_map(args.channel_map, args.input_channels, model_num_ch)
    muted_input_channels = parse_channel_list(args.mute_input_channels, args.input_channels)
    muted_selected_channels = [ch for ch in selected_channels if ch in muted_input_channels]
    if muted_selected_channels:
        raise ValueError(
            f"Muted channels {muted_selected_channels} cannot also be present in --channel-map {selected_channels}."
        )
    monitor_input_channel = (
        args.monitor_input_channel if args.monitor_input_channel is not None else selected_channels[0]
    )
    if monitor_input_channel < 0 or monitor_input_channel >= args.input_channels:
        raise ValueError(
            f"--monitor-input-channel {monitor_input_channel} is out of range for input_channels={args.input_channels}."
        )
    if monitor_input_channel in muted_input_channels:
        raise ValueError(
            f"--monitor-input-channel {monitor_input_channel} cannot be included in --mute-input-channels."
        )

    # Internal hop grouping to reduce callback churn while keeping the
    # ring-buffer-only output path. This is intentionally not exposed as a CLI.
    _INTERNAL_HOPS_PER_IO = 2
    model_io_chunk = model_chunk * _INTERNAL_HOPS_PER_IO
    input_block = int(round(model_io_chunk * input_sr / model_sr))
    output_block = int(round(model_io_chunk * output_sr / model_sr))
    if input_block <= 0 or output_block <= 0:
        raise ValueError("Invalid blocksize computed from input_sr/output_sr/model_sr.")

    print("=== Dummy Realtime Settings ===")
    print(f"run_dir: {args.run_dir}")
    print(f"config_path: {args.config_path}")
    print(f"checkpoint_path: {args.checkpoint_path}")
    print(f"model_device: {requested_device}")
    print(f"model_sr: {model_sr}, input_sr: {input_sr}, output_sr: {output_sr}")
    print(
        f"model_chunk: {model_chunk}, model_pad: {model_pad}, "
        f"input_block: {input_block}, output_block: {output_block}"
    )
    print(f"internal_hops_per_io: {_INTERNAL_HOPS_PER_IO}")
    print(f"model_num_ch: {model_num_ch}")
    print(f"input_channels: {args.input_channels}, output_channels: {args.output_channels}")
    print(f"channel_map: {selected_channels}, monitor_input_channel: {monitor_input_channel}")
    print(f"muted_input_channels: {muted_input_channels}")
    print(
        "input_preprocess: "
        f"{'off' if args.disable_input_preprocess else 'on'} "
        f"(glitch_threshold={args.glitch_threshold_dbfs}dBFS, "
        f"glitch_mute_ms={args.glitch_mute_ms}, "
        f"target_peak={args.target_input_peak_dbfs}dBFS, "
        f"max_gain={args.max_input_gain_db}dB, "
        f"activity_floor={args.activity_floor_dbfs}dBFS, "
        f"level_percentile={args.level_percentile})"
    )
    print(
        "simple_input_normalize: "
        f"{'on' if args.simple_input_normalize else 'off'} "
        f"(target_rms={args.simple_target_rms_dbfs}dBFS, max_gain={args.max_input_gain_db}dB)"
    )
    print(
        "fixed_input_gain_db: "
        f"{args.fixed_input_gain_db if args.fixed_input_gain_db is not None else 'off'}"
    )
    print(f"distance_threshold: {args.distance_threshold}")
    print(f"passthrough: {args.passthrough}, output_gain: {args.output_gain}, mix_dry: {args.mix_dry}")
    print(f"amp: {args.amp}")
    print("Press Ctrl+C to stop.")
    if args.input_channels > model_num_ch and args.channel_map is None:
        print(
            f"[audio-note] Input exposes {args.input_channels} channels; using the first {len(selected_channels)} "
            f"channels for the {model_num_ch}-channel model."
        )

    dis_embed = get_distance_embedding(args.distance_threshold, requested_device)

    state = model.init_buffers(1, requested_device)
    current_frame = torch.zeros(
        (1, model_num_ch, model_chunk + model_pad), dtype=torch.float32, device=requested_device
    )

    in_src = StreamingResampler(src_sr=input_sr, dst_sr=model_sr, num_channels=model_num_ch)
    out_src = StreamingResampler(src_sr=model_sr, dst_sr=output_sr, num_channels=1)
    print(f"[src] input={in_src.backend} output={out_src.backend}")
    if args.require_soxr and (in_src.backend != "soxr-stream" or out_src.backend != "soxr-stream"):
        raise SystemExit("Streaming SoXR backend required but unavailable.")

    # Warm up CUDA kernels before the audio stream opens so JIT compilation
    # and cuDNN algorithm selection don't stall the first real inferences.
    if requested_device.type == "cuda":
        torch.backends.cudnn.benchmark = True
        _N_WARMUP = 20
        print(f"Warming up CUDA ({_N_WARMUP} forward passes)...", end=" ", flush=True)
        _wu_t0 = time.time()
        _wu_state = model.init_buffers(1, requested_device)
        _wu_frame = torch.zeros_like(current_frame)
        with torch.no_grad():
            for _wi in range(_N_WARMUP):
                if args.amp:
                    with torch.amp.autocast("cuda", dtype=torch.float16):
                        _wu_out = model(
                            {"mixture": _wu_frame, "dis_embed": dis_embed},
                            input_state=_wu_state,
                            pad=False,
                        )
                else:
                    _wu_out = model(
                        {"mixture": _wu_frame, "dis_embed": dis_embed},
                        input_state=_wu_state,
                        pad=False,
                    )
                _wu_state = _wu_out["next_state"]
        torch.cuda.synchronize()
        _wu_ms = (time.time() - _wu_t0) * 1000.0
        print(f"done in {_wu_ms:.0f} ms ({_wu_ms/_N_WARMUP:.1f} ms/hop)")
        del _wu_state, _wu_frame, _wu_out
        torch.cuda.empty_cache()

    in_queue: queue.Queue[np.ndarray] = queue.Queue(maxsize=64)
    stop_event = threading.Event()
    ring_n_frames = max(output_block * 32, int(4.0 * output_sr))
    _RING_MAX_AHEAD_FRAMES = max(output_block * 3, int(0.10 * output_sr))
    _RING_TARGET_AHEAD_FRAMES = max(output_block, int(0.04 * output_sr))
    if _RING_TARGET_AHEAD_FRAMES >= _RING_MAX_AHEAD_FRAMES:
        _RING_TARGET_AHEAD_FRAMES = max(output_block, _RING_MAX_AHEAD_FRAMES // 2)

    output_ring = OutputRingBuffer(
        num_channels=args.output_channels,
        capacity_frames=ring_n_frames,
    )
    dry_ref = DryReferenceRing(capacity_frames=ring_n_frames, output_sr=output_sr, input_sr=input_sr)

    perf = {
        "n": 0,
        "avg_ms": 0.0,
        "in_rms": 0.0,
        "out_rms": 0.0,
        "in_drops": 0,
        "restarts": 0,
        "input_db": np.full(args.input_channels, -120.0, dtype=np.float32),
        "selected_db": np.full(len(selected_channels), -120.0, dtype=np.float32),
        "model_in_db": np.full(model_num_ch, -120.0, dtype=np.float32),
        "monitor_db": -120.0,
        "model_out_db": -120.0,
        "output_db": -120.0,
        "pre_gain_db": 0.0,
        "glitch_counts": np.zeros(args.input_channels, dtype=np.int32),
        "muted_glitch_samples": np.zeros(args.input_channels, dtype=np.int32),
        "last_level_print": time.monotonic(),
    }
    runtime_flags = {
        "input_exception": False,
        "output_exception": False,
        "input_status_seen": False,
        "output_status_seen": False,
        "worker_exception": False,
        "worker_exception_text": None,
    }

    def input_callback(indata, frames, _time_info, status):
        try:
            if status:
                runtime_flags["input_status_seen"] = True

            try:
                in_queue.put_nowait(indata.copy())
            except queue.Full:
                perf["in_drops"] += 1
        except Exception:
            runtime_flags["input_exception"] = True

    def output_callback(outdata, frames, _time_info, status):
        try:
            if status:
                runtime_flags["output_status_seen"] = True
            output_ring.read_into(outdata)
        except Exception:
            outdata[:] = 0.0
            runtime_flags["output_exception"] = True

    def worker():
        nonlocal state, current_frame
        try:
            while not stop_event.is_set():
                try:
                    in_block = in_queue.get(timeout=0.05)  # [T, C]
                except queue.Empty:
                    continue

                # Convert to [C, T]
                x_raw = in_block.T.astype(np.float32)
                if muted_input_channels:
                    x_raw[muted_input_channels, :] = 0.0

                pre_gain_db = 0.0
                if args.disable_input_preprocess:
                    perf["glitch_counts"] = np.zeros(args.input_channels, dtype=np.int32)
                    perf["muted_glitch_samples"] = np.zeros(args.input_channels, dtype=np.int32)
                elif args.simple_input_normalize:
                    # Normalization is deferred to after channel selection so that
                    # high-level reference/noise channels (e.g. ch0 at -3 dB) do
                    # not dominate the RMS and suppress gain on the mic channels.
                    perf["glitch_counts"] = np.zeros(args.input_channels, dtype=np.int32)
                    perf["muted_glitch_samples"] = np.zeros(args.input_channels, dtype=np.int32)
                else:
                    x_raw, pre_stats = _preprocess_live_input(
                        audio=x_raw,
                        sr=input_sr,
                        threshold_dbfs=args.glitch_threshold_dbfs,
                        mute_ms=args.glitch_mute_ms,
                        target_peak_dbfs=args.target_input_peak_dbfs,
                        max_gain_db=args.max_input_gain_db,
                        activity_floor_dbfs=args.activity_floor_dbfs,
                        level_percentile=args.level_percentile,
                    )
                    pre_gain_db += pre_stats["gain_db"]
                    perf["glitch_counts"] = pre_stats["raw_glitches"]
                    perf["muted_glitch_samples"] = pre_stats["muted_samples"]

                # Optional fixed gain stage applied after adaptive preprocessing.
                if args.fixed_input_gain_db is not None:
                    x_raw = _apply_fixed_input_gain(x_raw, args.fixed_input_gain_db)
                    pre_gain_db += float(args.fixed_input_gain_db)

                x = x_raw[selected_channels]
                x = _fit_channels(x, model_num_ch)
                x_ref = x_raw[monitor_input_channel].copy()

                # Feed a dry-reference ring at output_sr for timeline-consistent mixing.
                dry_dropped = dry_ref.push_input_block(x_ref)
                if dry_dropped > 0:
                    perf.setdefault("dry_ring_dropped_frames", 0)
                    perf["dry_ring_dropped_frames"] += int(dry_dropped)

                # Apply simple normalization on the selected/fitted channels only,
                # now that high-amplitude unrelated channels have been excluded.
                if not args.disable_input_preprocess and args.simple_input_normalize:
                    x, simple_gain_db = _simple_input_normalize(
                        audio=x,
                        target_rms_dbfs=args.simple_target_rms_dbfs,
                        max_gain_db=args.max_input_gain_db,
                    )
                    pre_gain_db += simple_gain_db

                perf["pre_gain_db"] = pre_gain_db

                input_db = np.asarray([_rms_dbfs(ch) for ch in x_raw], dtype=np.float32)
                selected_db = np.asarray([_rms_dbfs(ch) for ch in x[: len(selected_channels)]], dtype=np.float32)
                perf["input_db"] = _smooth_db(perf["input_db"], input_db)
                perf["selected_db"] = _smooth_db(perf["selected_db"], selected_db)
                perf["monitor_db"] = 0.8 * perf["monitor_db"] + 0.2 * _rms_dbfs(x_ref)
                perf["in_rms"] += (float(np.sqrt(np.mean(x_ref**2) + 1e-12)) - perf["in_rms"]) / max(1, perf["n"])

                x_ds = in_src.process(x)
                # Keep each worker iteration temporally consistent: exactly
                # _INTERNAL_HOPS_PER_IO model hops worth of samples.
                if x_ds.shape[-1] != model_io_chunk:
                    if x_ds.shape[-1] > model_io_chunk:
                        x_ds = x_ds[:, :model_io_chunk]
                    else:
                        pad = model_io_chunk - x_ds.shape[-1]
                        x_ds = np.pad(x_ds, ((0, 0), (0, pad)), mode="constant")

                # Exact per-channel levels of the tensor block fed to model forward.
                perf["model_in_db"] = np.asarray([_rms_dbfs(ch) for ch in x_ds], dtype=np.float32)
                if args.passthrough:
                    y_model = x_ds[0]
                    dt_ms = 0.0
                else:
                    with torch.no_grad():
                        t0 = time.time()
                        n_steps = max(1, int(np.ceil(x_ds.shape[-1] / model_chunk)))
                        # Accumulate GPU tensors; defer the GPU→CPU sync until after
                        # all hops are queued so the CUDA stream can pipeline them.
                        y_chunks_gpu: list = []
                        for step in range(n_steps):
                            start = step * model_chunk
                            end = min(start + model_chunk, x_ds.shape[-1])
                            x_step = x_ds[:, start:end]
                            valid = x_step.shape[-1]
                            if valid < model_chunk:
                                x_step = np.pad(
                                    x_step,
                                    ((0, 0), (0, model_chunk - valid)),
                                    mode="constant",
                                )

                            x_step_t = torch.from_numpy(x_step).to(requested_device)
                            current_frame[:, :, :-model_chunk] = current_frame[:, :, model_chunk:]
                            current_frame[0, :, -model_chunk:] = x_step_t
                            if args.amp and requested_device.type == "cuda":
                                with torch.amp.autocast("cuda", dtype=torch.float16):
                                    outputs = model(
                                        {"mixture": current_frame, "dis_embed": dis_embed},
                                        input_state=state,
                                        pad=False,
                                    )
                            else:
                                outputs = model(
                                    {"mixture": current_frame, "dis_embed": dis_embed},
                                    input_state=state,
                                    pad=False,
                                )
                            state = outputs["next_state"]
                            # Keep on GPU — no .cpu() here, avoids per-hop sync stall
                            y_chunks_gpu.append(outputs["output"][0, 0, :valid].detach().float())

                        # Single GPU→CPU transfer for all hops at once
                        y_model = torch.cat(y_chunks_gpu, dim=0).cpu().numpy()
                        dt_ms = (time.time() - t0) * 1000.0

            perf["n"] += 1
            # EMA (α=0.05, ≈20-sample window) so recent steady-state speed
            # dominates rather than slow CUDA warmup inferences.
            _ema_alpha = 0.05
            perf["avg_ms"] = _ema_alpha * dt_ms + (1.0 - _ema_alpha) * perf["avg_ms"]
            if perf["n"] % 200 == 0 and args.print_levels:
                ahead = output_ring.ahead_frames()
                print(
                    f"[perf] avg_infer_ms={perf['avg_ms']:.3f}, "
                    f"in_rms={perf['in_rms']:.5f}, out_rms={perf['out_rms']:.5f}, "
                    f"ring_ahead={ahead}, in_drops={perf['in_drops']}, underrun_frames={output_ring.stats.underrun_frames}"
                )

            # Resample model output from model_sr to the playback stream rate.
            perf["model_out_db"] = 0.8 * perf["model_out_db"] + 0.2 * _rms_dbfs(y_model)

            # Resample model output to output_sr.
            # One model hop in, one hop-equivalent at output_sr out.
            n_out = output_block
            y_io = out_src.process(y_model[None, :])[0]
            if y_io.shape[0] > n_out:
                y_io = y_io[:n_out]
            elif y_io.shape[0] < n_out:
                y_io = np.pad(y_io, (0, n_out - y_io.shape[0]), mode="constant")

            if args.mix_dry > 0:
                y_io = dry_ref.mix(y_wet=y_io, mix_dry=args.mix_dry, perf=perf)

            y_io = np.clip(y_io * args.output_gain, -1.0, 1.0)
        except Exception:
            runtime_flags["worker_exception"] = True
            runtime_flags["worker_exception_text"] = traceback.format_exc()
            perf["output_db"] = 0.8 * perf["output_db"] + 0.2 * _rms_dbfs(y_io)
            perf["out_rms"] += (float(np.sqrt(np.mean(y_io**2) + 1e-12)) - perf["out_rms"]) / max(1, perf["n"])

            y_out = _to_stereo_or_multich(y_io, args.output_channels)
            dropped = output_ring.write_drop_oldest(y_out)
            if dropped > 0:
                perf.setdefault("out_ring_dropped_frames", 0)
                perf["out_ring_dropped_frames"] += dropped

            ahead = output_ring.ahead_frames()
            if ahead > _RING_MAX_AHEAD_FRAMES:
                trim = ahead - _RING_TARGET_AHEAD_FRAMES
                dropped = output_ring.drop_oldest_frames(trim)
                if dropped > 0:
                    perf.setdefault("out_ring_dropped_frames", 0)
                    perf["out_ring_dropped_frames"] += dropped
                    dry_ref.drop_oldest_frames(dropped)

            now = time.monotonic()
            if args.print_levels and (now - perf["last_level_print"]) >= max(0.05, args.level_interval):
                ahead = output_ring.ahead_frames()
                print(
                    "[levels] "
                    f"{_format_db_list('usb=', perf['input_db'])} "
                    f"{_format_db_list('used=', perf['selected_db'])} "
                    f"{_format_db_list('mdl_in=', perf['model_in_db'])} "
                    f"mon={perf['monitor_db']:5.1f}dB "
                    f"model={perf['model_out_db']:5.1f}dB "
                    f"out={perf['output_db']:5.1f}dB "
                    f"pre={perf['pre_gain_db']:4.1f}dB "
                    f"glitch={int(np.sum(perf['glitch_counts']))} "
                    f"ring_ahead={ahead} "
                    f"avg_infer={perf['avg_ms']:.2f}ms "
                    f"drops={perf['in_drops']} underrun_frames={output_ring.stats.underrun_frames}"
                )
                perf["last_level_print"] = now

    worker_thread = threading.Thread(target=worker, daemon=True)
    worker_thread.start()

    try:
        input_device_info = sd.query_devices(args.input_device)
    except Exception:
        input_device_info = None

    try:
        output_device_info = sd.query_devices(args.output_device)
    except Exception:
        output_device_info = None

    if input_device_info is not None:
        print(
            "[audio-device] "
            f"input={args.input_device} "
            f"name={input_device_info['name']!r} "
            f"max_in={input_device_info['max_input_channels']} "
            f"default_sr={input_device_info['default_samplerate']}"
        )
    if output_device_info is not None:
        print(
            "[audio-device] "
            f"output={args.output_device} "
            f"name={output_device_info['name']!r} "
            f"max_out={output_device_info['max_output_channels']} "
            f"default_sr={output_device_info['default_samplerate']}"
        )

    try:
        sd.check_input_settings(
            device=args.input_device,
            channels=args.input_channels,
            samplerate=input_sr,
            dtype=np.float32,
        )
    except Exception as exc:
        print(f"[audio-config-error] input check failed: {exc}")
        print(
            "[audio-config] "
            f"requested input device={args.input_device}, channels={args.input_channels}, sr={input_sr}, dtype=float32"
        )
        print("Tip: verify the capture endpoint exposes this channel count to PortAudio, not just to Audacity.")
        stop_event.set()
        worker_thread.join(timeout=1.0)
        return

    try:
        sd.check_output_settings(
            device=args.output_device,
            channels=args.output_channels,
            samplerate=output_sr,
            dtype=np.float32,
        )
    except Exception as exc:
        print(f"[audio-config-error] output check failed: {exc}")
        print(
            "[audio-config] "
            f"requested output device={args.output_device}, channels={args.output_channels}, sr={output_sr}, dtype=float32"
        )
        print("Tip: choose a playback endpoint that accepts this sample rate/channel count.")
        stop_event.set()
        worker_thread.join(timeout=1.0)
        return

    input_stream = None
    output_stream = None
    try:
        input_stream = sd.InputStream(
            samplerate=input_sr,
            blocksize=input_block,
            channels=args.input_channels,
            dtype=np.float32,
            latency=args.latency,
            callback=input_callback,
            device=args.input_device,
        )
        output_stream = sd.OutputStream(
            samplerate=output_sr,
            blocksize=output_block,
            channels=args.output_channels,
            dtype=np.float32,
            latency=args.latency,
            callback=output_callback,
            device=args.output_device,
        )
        output_stream.start()
        input_stream.start()
        print("[audio] Input/output streams started.")
        while not stop_event.is_set():
            if runtime_flags["worker_exception"]:
                print("[worker-error]\n" + str(runtime_flags["worker_exception_text"]))
                stop_event.set()
                break
            if runtime_flags["input_exception"]:
                print("[audio] input callback exception")
                stop_event.set()
                break
            if runtime_flags["output_exception"]:
                print("[audio] output callback exception")
                stop_event.set()
                break
            if runtime_flags["input_status_seen"]:
                print("[audio] input callback reported status")
                runtime_flags["input_status_seen"] = False
            if runtime_flags["output_status_seen"]:
                print("[audio] output callback reported status")
                runtime_flags["output_status_seen"] = False
            if not input_stream.active:
                print("[audio] Input stream became inactive unexpectedly.")
                break
            if not output_stream.active:
                print("[audio] Output stream became inactive unexpectedly.")
                break
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("\nStopping stream...")
    except Exception as exc:
        print(f"[audio-runtime-error] {exc}")
        print(traceback.format_exc())
    finally:
        stop_event.set()
        if input_stream is not None:
            try:
                input_stream.stop()
            except Exception:
                pass
            try:
                input_stream.close()
            except Exception:
                pass
        if output_stream is not None:
            try:
                output_stream.stop()
            except Exception:
                pass
            try:
                output_stream.close()
            except Exception:
                pass
        worker_thread.join(timeout=1.0)


if __name__ == "__main__":
    main()
