from __future__ import annotations

import argparse
import os
import signal
import sys
import threading
import time
import traceback

import numpy as np

try:
    import sounddevice as sd
except ImportError:
    sd = None

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

import dummy_realtime as dr
from runtime_contract import load_contract

from edge.realtime.contracts import RealtimeContract
from edge.realtime.audio_io import create_streams, preflight_sounddevice
from edge.realtime.drop_oldest_buffer import DropOldestBuffer
from edge.realtime.ring_buffer import AudioRingBuffer, OutputRingBuffer
from edge.realtime.dry_reference import DryReferenceRing
from edge.realtime.scheduling import try_set_realtime_fifo

from edge.pipeline.onnx_streamer import ONNXStreamer
from edge.pipeline.processing import (
    ChannelLevelValidator,
    DCOffsetFilter,
    ProfileStats,
    dbfs_to_linear,
    render_monitor,
    StreamingResampler,
)
from edge.pipeline.runner import compute_output_ring_params


DEFAULT_INPUT_SR = 44100
DEFAULT_MODEL_SR = 24000
DEFAULT_OUTPUT_SR = 48000
DEFAULT_MODEL_CHANNELS = 6
DEFAULT_MODEL_CHUNK = 192
DEFAULT_MODEL_PAD = 96
DEFAULT_HOPS_PER_IO = 2
DEFAULT_ALPHA = 0.9999
DEFAULT_SILENCE_DBFS = -50.0
DEFAULT_FAULT_MIN_DBFS = -1.0
DEFAULT_FAULT_MAX_DBFS = 0.0
DEFAULT_PROFILE_INTERVAL = 1.0
DEFAULT_MONITOR_INTERVAL = 0.5
DEFAULT_MAX_OVERRUN_MS = 25.0

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Live Sound_Bubble ONNX inference pipeline.")
    parser.add_argument("--model", type=str, default=None, help="Exported ONNX path. Wins over --zoo-dir if both are given.")
    parser.add_argument("--contract-path", type=str, default=None, help="Runtime contract JSON generated at ONNX export.")
    parser.add_argument("--zoo-dir", type=str, default=None, help="Directory containing paired <name>.onnx + <name>.runtime.json files. Resolved against --bubble-radius via contract.dis_threshold.")
    parser.add_argument("--bubble-radius", type=float, default=None, help="Bubble radius in metres (e.g. 1.0, 1.5, 2.0). Required when using --zoo-dir.")
    parser.add_argument("--input-device", type=int, default=None)
    parser.add_argument("--output-device", type=int, default=None)
    parser.add_argument("--input-channels", type=int, default=8)
    parser.add_argument("--output-channels", type=int, default=2)
    parser.add_argument("--channel-map", type=str, default="0,1,2,3,4,5")
    parser.add_argument("--monitor-input-channel", type=int, default=0)
    parser.add_argument("--input-sr", type=int, default=DEFAULT_INPUT_SR)
    parser.add_argument("--model-sr", type=int, default=DEFAULT_MODEL_SR)
    parser.add_argument("--output-sr", type=int, default=DEFAULT_OUTPUT_SR)
    parser.add_argument("--model-chunk", type=int, default=DEFAULT_MODEL_CHUNK)
    parser.add_argument("--model-pad", type=int, default=DEFAULT_MODEL_PAD)
    parser.add_argument("--model-num-ch", type=int, default=DEFAULT_MODEL_CHANNELS)
    parser.add_argument("--hops-per-io", type=int, default=DEFAULT_HOPS_PER_IO)
    parser.add_argument("--latency", type=str, default="low", choices=["low", "high"])
    parser.add_argument("--list-devices", action="store_true")
    # Real-time contract knobs. These are the key anti-accumulation controls.
    # Keep capture buffering shallow; drop oldest when overloaded (see worker loop).
    parser.add_argument("--max-capture-blocks", type=int, default=2, help="Max capture blocks to buffer (drop-oldest).")
    parser.add_argument("--max-output-ahead-sec", type=float, default=0.10, help="Max output buffer ahead before dropping oldest audio.")
    parser.add_argument("--target-output-ahead-sec", type=float, default=0.04, help="Target output buffer ahead after drop/trim.")
    parser.add_argument("--rt-fifo-priority", type=int, default=0, help="Best-effort set SCHED_FIFO for this process (0 disables).")
    parser.add_argument("--output-gain", type=float, default=1.0)
    parser.add_argument("--mix-dry", type=float, default=0.0)
    parser.add_argument("--input-gain-db", type=float, default=0.0)
    parser.add_argument("--alpha", type=float, default=DEFAULT_ALPHA, help="DC IIR alpha.")
    parser.add_argument("--silence-dbfs", type=float, default=DEFAULT_SILENCE_DBFS)
    parser.add_argument(
        "--silence-window-frames",
        type=int,
        default=8,
        help="Rolling window (frames) used for silence/dead-mic detection.",
    )
    parser.add_argument("--fault-min-dbfs", type=float, default=DEFAULT_FAULT_MIN_DBFS)
    parser.add_argument("--fault-max-dbfs", type=float, default=DEFAULT_FAULT_MAX_DBFS)
    parser.add_argument("--max-overrun-ms", type=float, default=DEFAULT_MAX_OVERRUN_MS)
    parser.add_argument("--intra-op-threads", type=int, default=2)
    parser.add_argument("--inter-op-threads", type=int, default=1)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--profile-interval", type=float, default=DEFAULT_PROFILE_INTERVAL)
    parser.add_argument("--monitor", action="store_true", help="Print live per-channel dBFS and status.")
    parser.add_argument("--monitor-interval", type=float, default=DEFAULT_MONITOR_INTERVAL)
    # Worker backlog guardrails (prevents seconds-long accumulation + OOM).
    parser.add_argument("--model-ring-ms", type=float, default=64.0, help="Model input ring capacity in milliseconds at --model-sr (drop-oldest).")
    parser.add_argument("--max-steps-per-block", type=int, default=4, help="Cap catch-up inference steps per capture block (prefer dropout over backlog growth on RPi).")
    parser.add_argument(
        "--queue-policy",
        type=str,
        default="adaptive",
        choices=["fifo", "latest", "adaptive"],
        help="Capture queue service discipline. 'adaptive' uses latest-wins under overload.",
    )
    parser.add_argument(
        "--max-capture-age-ms",
        type=float,
        default=24.0,
        help="If oldest queued capture is older than this, treat it as stale and shed to newest.",
    )
    parser.add_argument(
        "--recover-steps-per-block",
        type=int,
        default=2,
        help="Temporary worker step budget when output ahead is low but capture is still fresh.",
    )
    parser.add_argument(
        "--recover-ahead-multiplier",
        type=float,
        default=1.5,
        help="Temporary multiplier on target output-ahead during recovery mode.",
    )
    parser.add_argument(
        "--overload-policy",
        type=str,
        default="dry_fill",
        choices=["dry_fill", "silence_fill", "strict"],
        help=(
            "When full wet inference for the current block is not affordable: "
            "'dry_fill' fills missing output time with dry audio and resets model state after write, "
            "'silence_fill' fills with silence and resets after write, "
            "'strict' keeps current behavior (no synthetic fill)."
        ),
    )
    parser.add_argument("--require-soxr", action="store_true", help="Fail if streaming SoXR backend is unavailable.")
    return parser.parse_args()

def _resolve_zoo_model(zoo_dir: str, bubble_radius: float) -> tuple[str, str, list[float]]:
    """Scan zoo_dir for paired <name>.onnx + <name>.runtime.json. Match contract.dis_threshold
    to bubble_radius. Returns (onnx_path, contract_path, available_radii)."""
    from pathlib import Path as _P
    zoo = _P(zoo_dir)
    if not zoo.is_dir():
        raise FileNotFoundError(f"--zoo-dir does not exist or is not a directory: {zoo_dir}")
    # Entries are tuples of (offered_radii, contract_path, onnx_path). A single-
    # distance entry offers {dis_threshold}; a distance-aware entry offers
    # supported_radii (one model serves multiple radii — used as fallback).
    found: list[tuple[set[float], _P, _P]] = []
    for contract_path in sorted(zoo.glob("*.runtime.json")):
        try:
            c = load_contract(contract_path)
        except Exception as exc:
            print(f"[zoo] skipping {contract_path.name}: load failed ({exc})")
            continue
        onnx_stem = contract_path.name[: -len(".runtime.json")]
        onnx_path = zoo / f"{onnx_stem}.onnx"
        if not onnx_path.is_file():
            print(f"[zoo] skipping {contract_path.name}: paired {onnx_path.name} not found")
            continue
        offered: set[float] = set()
        if c.dis_threshold is not None:
            offered.add(float(c.dis_threshold))
        if c.supported_radii:
            offered.update(float(r) for r in c.supported_radii)
        if not offered:
            print(f"[zoo] skipping {contract_path.name}: no dis_threshold / supported_radii (re-export to record)")
            continue
        found.append((offered, contract_path, onnx_path))
    available = sorted({r for offered, _, _ in found for r in offered})
    if not found:
        raise RuntimeError(f"No valid zoo entries found in {zoo_dir}")
    # Prefer single-distance exact match over distance-aware (more specific).
    single = [(offered, c, o) for offered, c, o in found if len(offered) == 1 and bubble_radius in offered]
    multi  = [(offered, c, o) for offered, c, o in found if len(offered) > 1 and bubble_radius in offered]
    picks = single + multi
    if not picks:
        raise RuntimeError(
            f"--bubble-radius {bubble_radius} not offered by any zoo entry in {zoo_dir}. Available: {available}"
        )
    _, contract_path, onnx_path = picks[0]
    return str(onnx_path), str(contract_path), available


def main() -> None:
    args = parse_args()

    # Build and validate the real-time contract as early as possible.
    # This is the primary guardrail against unbounded latency accumulation.
    rt_contract = RealtimeContract(
        max_capture_blocks=int(args.max_capture_blocks),
        max_output_ahead_sec=float(args.max_output_ahead_sec),
        target_output_ahead_sec=float(args.target_output_ahead_sec),
        rt_fifo_priority=int(args.rt_fifo_priority),
    ).validate()
    if rt_contract.rt_fifo_priority > 0:
        res = try_set_realtime_fifo(rt_contract.rt_fifo_priority)
        if not res.applied:
            print(f"[warn] rt scheduling not applied: {res.message}")
        else:
            print(f"[rt] {res.requested_policy} prio={res.requested_priority} applied")

    # Model/zoo selection rules:
    #   --model (+ --contract-path)        -> legacy single-ONNX path; highest priority
    #   --zoo-dir + --bubble-radius        -> zoo resolution by contract.dis_threshold
    #   --bubble-radius alone              -> OK: applies to distance-aware ONNX at runtime
    zoo_requested = args.zoo_dir is not None
    if zoo_requested and args.bubble_radius is None:
        raise SystemExit("--zoo-dir requires --bubble-radius to select which model to load.")
    if args.model is None and not zoo_requested:
        raise SystemExit("Provide --model (+ optional --contract-path) or --zoo-dir + --bubble-radius.")
    if args.model is not None and zoo_requested:
        print(f"[zoo] --model given explicitly; ignoring --zoo-dir (--bubble-radius retained for distance-aware ONNX).")
    elif zoo_requested:
        onnx_path, contract_path, available = _resolve_zoo_model(args.zoo_dir, args.bubble_radius)
        args.model = onnx_path
        args.contract_path = contract_path
        print(f"[zoo] dir={args.zoo_dir} available_radii={available} selected={args.bubble_radius}m")

    contract = None
    if args.contract_path:
        contract = load_contract(args.contract_path)
        args.model_sr = contract.model_sr
        args.model_num_ch = contract.model_num_ch
        args.model_chunk = contract.model_chunk
        args.model_pad = contract.model_pad
        if args.channel_map == "0,1,2,3,4,5":
            args.channel_map = ",".join(str(x) for x in contract.expected_channel_map)
        if args.input_channels == 8:
            args.input_channels = contract.expected_input_channels

    if args.list_devices:
        dr.list_audio_devices()
        return

    if sd is None:
        raise RuntimeError("Install sounddevice first: pip install sounddevice")

    model_frame_len = int(args.model_chunk + args.model_pad)
    model_io_chunk = int(args.model_chunk * max(1, args.hops_per_io))
    input_block = int(round(model_io_chunk * args.input_sr / args.model_sr))
    output_block = int(round(model_io_chunk * args.output_sr / args.model_sr))
    if input_block <= 0 or output_block <= 0:
        raise ValueError("Computed block size is invalid.")

    selected_channels = dr.parse_channel_map(args.channel_map, args.input_channels, args.model_num_ch)
    monitor_channel = int(args.monitor_input_channel)
    if monitor_channel < 0 or monitor_channel >= args.input_channels:
        raise ValueError("--monitor-input-channel out of range.")

    print("=== Sound Bubble ONNX Live Pipeline ===")
    print(f"model={args.model}")
    if contract is not None:
        print(f"contract={args.contract_path}")
        prov = []
        if contract.model_class:
            prov.append(f"class={contract.model_class.rsplit('.', 1)[-1]}")
        if contract.supported_radii:
            prov.append(f"supported_radii={[f'{r:g}m' for r in contract.supported_radii]}")
        elif contract.dis_threshold is not None:
            prov.append(f"bubble_radius={contract.dis_threshold:g}m")
        if contract.trained_epochs is not None:
            prov.append(f"epochs={contract.trained_epochs}")
        if prov:
            print("provenance: " + ", ".join(prov))
        if contract.source_config:
            print(f"source_config={contract.source_config}")
        if contract.source_run_dir:
            print(f"source_run_dir={contract.source_run_dir}")
    print(f"input_sr={args.input_sr}, model_sr={args.model_sr}, output_sr={args.output_sr}")
    print(f"input_block={input_block}, output_block={output_block}, model_chunk={args.model_chunk}")
    print(f"channel_map={selected_channels}, monitor={args.monitor}")

    onnx_streamer = ONNXStreamer(args.model, args.intra_op_threads, args.inter_op_threads)
    if onnx_streamer.accepts_dis_embed:
        # Distance-aware ONNX: radius must be known and fed every frame. If the
        # user passed --bubble-radius, honour it; otherwise fall back to
        # contract.dis_threshold, then supported_radii[0] (with a loud warning
        # because the choice is silently baked in for the whole session).
        chosen_radius: float | None = args.bubble_radius
        source = "--bubble-radius"
        if chosen_radius is None and contract is not None:
            if contract.dis_threshold is not None:
                chosen_radius = float(contract.dis_threshold)
                source = "contract.dis_threshold"
            elif contract.supported_radii:
                chosen_radius = float(contract.supported_radii[0])
                source = f"contract.supported_radii[0] (options: {contract.supported_radii})"
                print(f"[warn] distance-aware model loaded with no --bubble-radius; defaulting to {chosen_radius}m. Pass --bubble-radius to silence this.")
        if chosen_radius is None:
            raise SystemExit(
                "Loaded ONNX is distance-aware (has dis_embed input) but no radius was provided. "
                "Pass --bubble-radius {1.0|1.5|2.0}."
            )
        onnx_streamer.set_radius(chosen_radius)
        print(f"[model] distance-aware — active bubble_radius={chosen_radius:g}m (source: {source})")
    elif args.bubble_radius is not None and not zoo_requested:
        # User passed --bubble-radius to a single-distance model; informational only.
        print(f"[model] --bubble-radius {args.bubble_radius} ignored (loaded model is single-distance).")
    dc_filter = DCOffsetFilter(num_channels=args.model_num_ch, alpha=args.alpha)
    level_validator = ChannelLevelValidator(
        num_channels=args.model_num_ch,
        silence_dbfs=args.silence_dbfs,
        fault_min_dbfs=args.fault_min_dbfs,
        fault_max_dbfs=args.fault_max_dbfs,
        silence_window_frames=args.silence_window_frames,
    )

    # Capture buffering is intentionally shallow and drop-oldest. This prevents
    # unbounded latency accumulation: overload turns into controlled dropout.
    in_buf: DropOldestBuffer[dict[str, object]] = DropOldestBuffer(capacity=rt_contract.max_capture_blocks)
    stop_event = threading.Event()
    perf = {
        "out_ring_dropped_frames": 0,
        "model_ring_drops": 0,
        "model_ring_resets": 0,
        "capture_discontinuities": 0,
        "capture_gap_blocks": 0,
        "capture_drained_blocks": 0,
        "queue_mode_changes": 0,
        "last_profile": time.monotonic(),
        "last_monitor": time.monotonic(),
    }
    runtime_flags = {
        "input_status_seen": False,
        "output_status_seen": False,
        "input_exception": False,
        "output_exception": False,
        "worker_exception": False,
        "worker_exception_text": None,
    }
    profile_ema = ProfileStats()
    latest_levels = np.full(args.model_num_ch, -120.0, dtype=np.float32)
    latest_statuses = ["ok"] * args.model_num_ch
    queue_policy = {
        "mode": "normal",
        "last_capture_idx": None,
        "last_steps_run": 0,
        "last_capture_age_ms": 0.0,
        "last_capture_gap": 0,
        "last_capture_drained": 0,
        "last_infer_total_ms": 0.0,
        "infer_ema_ms": 8.5,
        "dc_ema_ms": 0.1,
        "level_ema_ms": 0.2,
        "frame_to_infer_ema_ms": 0.6,
        "post_ema_ms": 0.2,
    }

    ring_params = compute_output_ring_params(
        args.output_sr,
        output_block,
        max_ahead_sec=rt_contract.max_output_ahead_sec,
        target_ahead_sec=rt_contract.target_output_ahead_sec,
    )
    ring_n_frames = ring_params.ring_n_frames
    output_ring = OutputRingBuffer(
        num_channels=args.output_channels,
        capacity_frames=ring_n_frames,
    )
    dry_ref = DryReferenceRing(capacity_frames=ring_n_frames, output_sr=args.output_sr, input_sr=args.input_sr)

    probe_in_src = StreamingResampler(src_sr=args.input_sr, dst_sr=args.model_sr, num_channels=args.model_num_ch)
    probe_out_src = StreamingResampler(src_sr=args.model_sr, dst_sr=args.output_sr, num_channels=1)
    print(f"[src] input={probe_in_src.backend} output={probe_out_src.backend}")
    if args.require_soxr and (probe_in_src.backend != "soxr-stream" or probe_out_src.backend != "soxr-stream"):
        raise SystemExit("Streaming SoXR backend required but unavailable.")
    capture_frame_index = [0]
    proc_frame_index = [0]
    last_output_chunk = [np.zeros(args.model_chunk, dtype=np.float32)]

    def request_stop(reason: str) -> None:
        if not stop_event.is_set():
            print(f"[shutdown] {reason}")
            stop_event.set()
            try:
                in_buf.close()
            except Exception:
                pass

    def input_callback(indata, _frames, _time_info, status) -> None:
        try:
            if status:
                runtime_flags["input_status_seen"] = True
            item = {
                "x": indata.copy(),
                "t_capture": time.perf_counter(),
                "capture_idx": capture_frame_index[0],
            }
            capture_frame_index[0] += 1
            in_buf.push(item)
        except Exception:
            runtime_flags["input_exception"] = True

    def output_callback(outdata, frames, _time_info, status) -> None:
        try:
            if status:
                runtime_flags["output_status_seen"] = True
            output_ring.read_into(outdata)
        except Exception:
            outdata[:] = 0.0
            runtime_flags["output_exception"] = True

    def worker() -> None:
        try:
            current_frame = np.zeros((1, args.model_num_ch, model_frame_len), dtype=np.float32)
            ring_ms = float(args.model_ring_ms)
            ring_capacity = max(1, int(round((ring_ms / 1000.0) * float(args.model_sr))))
            model_ring = AudioRingBuffer(num_channels=args.model_num_ch, capacity_samples=ring_capacity)
            in_src = StreamingResampler(src_sr=args.input_sr, dst_sr=args.model_sr, num_channels=args.model_num_ch)
            out_src = StreamingResampler(src_sr=args.model_sr, dst_sr=args.output_sr, num_channels=1)

            def reset_stream_timeline(reason: str, *, clear_io_rings: bool = True) -> None:
                _ = reason
                perf["capture_discontinuities"] += 1
                in_src.reset()
                out_src.reset()
                onnx_streamer.reset_state_and_warm()
                current_frame.fill(0.0)
                last_output_chunk[0].fill(0.0)
                if hasattr(model_ring, "clear"):
                    model_ring.clear()
                if clear_io_rings:
                    output_ring.clear()
                    dry_ref.clear()

            def consume_dry_output_frames(n_frames: int) -> np.ndarray:
                dry = np.zeros((max(0, int(n_frames)), args.output_channels), dtype=np.float32)
                if dry.shape[0] <= 0:
                    return dry
                if hasattr(dry_ref, "read_into_output"):
                    got = int(dry_ref.read_into_output(dry))
                    if got < dry.shape[0]:
                        dry[got:, :] = 0.0
                    return dry
                return dry

            while not stop_event.is_set():
                pending_timeline_reset_reason: str | None = None
                if args.queue_policy == "fifo":
                    item = in_buf.pop(timeout=0.05)
                    drained_blocks = 0
                else:
                    item, drained_blocks = in_buf.pop_latest(timeout=0.05)
                if item is None:
                    continue

                in_block = item["x"]
                captured_ts = float(item["t_capture"])
                frame_idx = int(item["capture_idx"])
                proc_frame_index[0] = frame_idx

                capture_age_ms = (time.perf_counter() - captured_ts) * 1000.0
                capture_gap = 0
                if queue_policy["last_capture_idx"] is not None:
                    capture_gap = max(0, int(frame_idx - int(queue_policy["last_capture_idx"]) - 1))

                capture_discontinuity = False
                if drained_blocks > 0:
                    capture_discontinuity = True
                    perf["capture_drained_blocks"] += int(drained_blocks)
                if capture_gap > 0:
                    capture_discontinuity = True
                    perf["capture_gap_blocks"] += int(capture_gap)
                if args.queue_policy == "adaptive" and capture_age_ms > args.max_capture_age_ms:
                    capture_discontinuity = True

                queue_policy["last_capture_age_ms"] = float(capture_age_ms)
                queue_policy["last_capture_gap"] = int(capture_gap)
                queue_policy["last_capture_drained"] = int(drained_blocks)

                if capture_discontinuity:
                    if queue_policy["mode"] != "shed":
                        perf["queue_mode_changes"] += 1
                    queue_policy["mode"] = "shed"
                    pending_timeline_reset_reason = "capture-discontinuity"
                else:
                    if queue_policy["mode"] == "shed":
                        perf["queue_mode_changes"] += 1
                    queue_policy["mode"] = "normal"

                queue_policy["last_capture_idx"] = int(frame_idx)
                stage = ProfileStats()
                t_start = time.perf_counter()
                stage.capture_to_frame_ms = (t_start - captured_ts) * 1000.0

                x_raw = in_block.T.astype(np.float32)
                x_ref = x_raw[monitor_channel].copy()
                x = x_raw[selected_channels]
                x = dr._fit_channels(x, args.model_num_ch)
                t_dc0 = time.perf_counter()
                x_dc = dc_filter.process(x)
                stage.dc_ms = (time.perf_counter() - t_dc0) * 1000.0
                t_lv0 = time.perf_counter()
                x_valid, levels, statuses = level_validator.validate(x_dc, frame_idx=frame_idx)
                stage.level_ms = (time.perf_counter() - t_lv0) * 1000.0
                latest_levels[:] = levels
                latest_statuses[:] = statuses

                # Keep a dry-reference ring at output_sr so dry/wet always shares a timeline.
                # This avoids silent truncation when catch-up produces multiple model chunks.
                dry_dropped = dry_ref.push_input_block(x_ref)
                if dry_dropped > 0:
                    perf.setdefault("dry_ring_dropped_frames", 0)
                    perf["dry_ring_dropped_frames"] += int(dry_dropped)

                gain = dbfs_to_linear(args.input_gain_db)
                if gain != 1.0:
                    x_valid = np.clip(x_valid * gain, -1.0, 1.0)

                stage.drift_ppm = 0.0
                x_model = in_src.process(x_valid)

                before_drops = model_ring.stats.dropped_samples
                model_ring.push(x_model)
                dropped_now = model_ring.stats.dropped_samples - before_drops
                if dropped_now > 0:
                    perf["model_ring_drops"] += 1
                    reset_stream_timeline("model-ring-drop", clear_io_rings=True)
                    perf["model_ring_resets"] += 1
                y_chunks: list[np.ndarray] = []

                out_ahead_ms = 1000.0 * float(output_ring.ahead_frames()) / float(args.output_sr)
                target_ahead_ms = 1000.0 * float(ring_params.ring_target_ahead_frames) / float(args.output_sr)

                block_budget_ms = 1000.0 * float(input_block) / float(args.input_sr)
                infer_ema_ms = max(0.001, float(queue_policy.get("infer_ema_ms", 8.5)))
                noninfer_ms = (
                    float(queue_policy.get("dc_ema_ms", 0.1))
                    + float(queue_policy.get("level_ema_ms", 0.2))
                    + float(queue_policy.get("frame_to_infer_ema_ms", 0.5))
                    + float(queue_policy.get("post_ema_ms", 0.2))
                )
                reserve_ms = 1.0
                safe_budget_ms = max(0.0, block_budget_ms - noninfer_ms - reserve_ms)
                sustainable_steps = max(0, int(safe_budget_ms // infer_ema_ms))
                steady_target_steps = min(int(args.hops_per_io), int(sustainable_steps), int(args.max_steps_per_block))

                recover_target = min(int(args.recover_steps_per_block), int(args.hops_per_io), int(args.max_steps_per_block))

                need_recover = (
                    args.queue_policy == "adaptive"
                    and queue_policy["mode"] != "shed"
                    and float(queue_policy["last_capture_age_ms"]) < 0.5 * args.max_capture_age_ms
                    and out_ahead_ms < target_ahead_ms
                )

                if queue_policy["mode"] == "shed":
                    step_budget = steady_target_steps
                    effective_target_ahead_ms = target_ahead_ms
                elif need_recover and sustainable_steps >= recover_target:
                    if queue_policy["mode"] != "recover":
                        perf["queue_mode_changes"] += 1
                    queue_policy["mode"] = "recover"
                    step_budget = recover_target
                    effective_target_ahead_ms = target_ahead_ms * float(args.recover_ahead_multiplier)
                else:
                    if queue_policy["mode"] == "recover":
                        perf["queue_mode_changes"] += 1
                    if queue_policy["mode"] != "shed":
                        queue_policy["mode"] = "normal"
                    step_budget = steady_target_steps
                    effective_target_ahead_ms = target_ahead_ms

                infer_start = time.perf_counter()
                infer_times: list[float] = []
                steps = 0
                while steps < step_budget:
                    step = model_ring.pop(args.model_chunk)
                    if step is None:
                        break
                    current_frame[:, :, :-args.model_chunk] = current_frame[:, :, args.model_chunk:]
                    current_frame[0, :, -args.model_chunk:] = step

                    run_res = onnx_streamer.run(current_frame)
                    infer_times.append(run_res.infer_ms)
                    y_chunk = run_res.output[0, 0, : args.model_chunk].astype(np.float32, copy=False)
                    steps += 1

                    if run_res.infer_ms > args.max_overrun_ms:
                        perf.setdefault("overruns", 0)
                        perf["overruns"] += 1

                    last_output_chunk[0] = y_chunk.copy()
                    y_chunks.append(y_chunk)

                stage.frame_to_infer_ms = (infer_start - t_start) * 1000.0
                stage.infer_ms = float(np.mean(infer_times)) if infer_times else 0.0

                t_post0 = time.perf_counter()
                steps_run = len(infer_times)
                missing_steps = max(0, int(args.hops_per_io) - int(steps_run))
                frames_per_hop_out = int(round(float(args.output_sr) * (float(args.model_chunk) / float(args.model_sr))))
                y_out: np.ndarray | None = None

                if y_chunks:
                    y_model = np.concatenate(y_chunks, axis=0)
                    y_io = out_src.process(y_model[None, :])[0]
                    if args.mix_dry > 0.0:
                        y_io = dry_ref.mix(y_wet=y_io, mix_dry=args.mix_dry, perf=perf)
                    y_io = np.clip(y_io * args.output_gain, -1.0, 1.0)
                    y_out = dr._to_stereo_or_multich(y_io.astype(np.float32, copy=False), args.output_channels)
                else:
                    y_out = np.zeros((0, int(args.output_channels)), dtype=np.float32)

                if missing_steps > 0 and args.overload_policy != "strict":
                    missing_frames = int(missing_steps) * int(frames_per_hop_out)
                    if args.overload_policy == "dry_fill":
                        y_fill = consume_dry_output_frames(missing_frames)
                    else:
                        y_fill = np.zeros((missing_frames, int(args.output_channels)), dtype=np.float32)

                    if y_out is None or y_out.shape[0] == 0:
                        y_out = y_fill
                    else:
                        y_out = np.concatenate([y_out, y_fill], axis=0)

                    if pending_timeline_reset_reason is None:
                        pending_timeline_reset_reason = "missing-wet-hops-filled"
                    else:
                        pending_timeline_reset_reason = "capture-discontinuity"

                if y_out is not None and y_out.shape[0] > 0:
                    dropped = output_ring.write_drop_oldest(y_out)
                    if dropped > 0:
                        perf["out_ring_dropped_frames"] += dropped

                    ahead = output_ring.ahead_frames()
                    if ahead > ring_params.ring_max_ahead_frames:
                        effective_target_ahead_frames = int(effective_target_ahead_ms * args.output_sr / 1000.0)
                        trim = ahead - effective_target_ahead_frames
                        dropped = output_ring.drop_oldest_frames(trim)
                        if dropped > 0:
                            perf["out_ring_dropped_frames"] += dropped
                            dry_ref.drop_oldest_frames(dropped)

                if pending_timeline_reset_reason is not None:
                    # Defer clearing output/dry rings until after this block is queued; keep
                    # already-written playback audio intact.
                    reset_stream_timeline(pending_timeline_reset_reason, clear_io_rings=False)

                stage.post_ms = (time.perf_counter() - t_post0) * 1000.0
                stage.e2e_ms = (time.perf_counter() - captured_ts) * 1000.0
                infer_total_ms = float(sum(infer_times)) if steps_run > 0 else 0.0
                infer_mean_ms = float(infer_total_ms / max(1, steps_run))
                queue_policy["last_steps_run"] = int(steps_run)
                queue_policy["last_infer_total_ms"] = float(infer_total_ms)

                _ema_alpha = 0.2
                if steps_run > 0:
                    queue_policy["infer_ema_ms"] = (1.0 - _ema_alpha) * float(queue_policy["infer_ema_ms"]) + _ema_alpha * float(
                        infer_mean_ms
                    )
                queue_policy["dc_ema_ms"] = (1.0 - _ema_alpha) * float(queue_policy["dc_ema_ms"]) + _ema_alpha * float(stage.dc_ms)
                queue_policy["level_ema_ms"] = (1.0 - _ema_alpha) * float(queue_policy["level_ema_ms"]) + _ema_alpha * float(
                    stage.level_ms
                )
                queue_policy["frame_to_infer_ema_ms"] = (1.0 - _ema_alpha) * float(
                    queue_policy["frame_to_infer_ema_ms"]
                ) + _ema_alpha * float(stage.frame_to_infer_ms)
                queue_policy["post_ema_ms"] = (1.0 - _ema_alpha) * float(queue_policy["post_ema_ms"]) + _ema_alpha * float(stage.post_ms)

                profile_ema.update(stage)

                now = time.monotonic()
                if args.profile and (now - perf["last_profile"]) >= max(0.1, args.profile_interval):
                    out_ahead_frames = output_ring.ahead_frames()
                    out_ahead_ms = (out_ahead_frames / float(args.output_sr)) * 1000.0
                    display_mode = str(queue_policy["mode"])
                    if int(steps_run) == 0 and float(out_ahead_ms) <= 0.1:
                        display_mode = "starved"
                    model_ring_ms = (model_ring.size() / float(args.model_sr)) * 1000.0
                    print(
                        "[profile] "
                        f"mode={display_mode}  "
                        f"capture_age={queue_policy['last_capture_age_ms']:.2f}ms  "
                        f"capture_gap={queue_policy['last_capture_gap']}  "
                        f"capture_drained={queue_policy['last_capture_drained']}  "
                        f"capture->frame={stage.capture_to_frame_ms:.2f}ms  "
                        f"dc={stage.dc_ms:.2f}ms  "
                        f"level-valid={stage.level_ms:.2f}ms  "
                        f"frame->infer={stage.frame_to_infer_ms:.2f}ms  "
                        f"infer_mean={infer_mean_ms:.2f}ms  "
                        f"infer_total={infer_total_ms:.2f}ms  "
                        f"steps={steps_run}  "
                        f"post+out={stage.post_ms:.2f}ms  "
                        f"e2e={stage.e2e_ms:.2f}ms  "
                        f"drift={stage.drift_ppm:+.2f}ppm  "
                        f"q={in_buf.qsize()}/{args.max_capture_blocks}  "
                        f"drops_oldest={in_buf.stats.dropped_oldest}  "
                        f"out_ahead={out_ahead_ms:.1f}ms dropped={perf['out_ring_dropped_frames']} underrun_frames={output_ring.stats.underrun_frames}  "
                        f"model_ring={model_ring_ms:.1f}ms drops={perf['model_ring_drops']} resets={perf['model_ring_resets']}"
                    )
                    perf["last_profile"] = now

                if args.monitor and (now - perf["last_monitor"]) >= max(0.1, args.monitor_interval):
                    print(render_monitor(latest_levels, latest_statuses))
                    perf["last_monitor"] = now
        except Exception:
            runtime_flags["worker_exception"] = True
            runtime_flags["worker_exception_text"] = traceback.format_exc()

    worker_thread = threading.Thread(target=worker, daemon=False)
    worker_thread.start()

    def _signal_handler(sig, _frame) -> None:
        try:
            sig_name = signal.Signals(sig).name
        except Exception:
            sig_name = str(sig)
        request_stop(f"signal:{sig_name}")

    if threading.current_thread() is threading.main_thread():
        signal.signal(signal.SIGINT, _signal_handler)
        if hasattr(signal, "SIGTERM"):
            signal.signal(signal.SIGTERM, _signal_handler)

    def cleanup(stream_in, stream_out) -> None:
        request_stop("cleanup")
        for stream in (stream_in, stream_out):
            if stream is not None:
                try:
                    stream.stop()
                except Exception:
                    pass
                try:
                    stream.close()
                except Exception:
                    pass
        worker_thread.join(timeout=3.0)
        if worker_thread.is_alive():
            print("[shutdown-warn] worker thread did not exit in time")
        print("[summary] silence_counts=", level_validator.silence_counts.tolist())
        print("[summary] fault_counts=", level_validator.fault_counts.tolist())

    try:
        preflight_sounddevice(
            sd,
            input_device=args.input_device,
            input_channels=args.input_channels,
            input_sr=args.input_sr,
            output_device=args.output_device,
            output_channels=args.output_channels,
            output_sr=args.output_sr,
        )
    except Exception as exc:
        print(f"[audio-config-error] {exc}")
        cleanup(None, None)
        return

    input_stream = None
    output_stream = None
    try:
        streams = create_streams(
            sd,
            input_sr=args.input_sr,
            input_block=input_block,
            input_channels=args.input_channels,
            input_latency=args.latency,
            input_callback=input_callback,
            input_device=args.input_device,
            output_sr=args.output_sr,
            output_block=output_block,
            output_channels=args.output_channels,
            output_latency=args.latency,
            output_callback=output_callback,
            output_device=args.output_device,
        )
        input_stream = streams.input_stream
        output_stream = streams.output_stream
        output_stream.start()
        input_stream.start()
        print("[audio] streams started, Ctrl+C to stop")
        while not stop_event.is_set():
            if runtime_flags["worker_exception"]:
                print("[worker-error]\n" + str(runtime_flags["worker_exception_text"]))
                request_stop("worker exception")
                break
            if runtime_flags["input_exception"]:
                request_stop("input callback exception")
                break
            if runtime_flags["output_exception"]:
                request_stop("output callback exception")
                break
            if runtime_flags["input_status_seen"]:
                print("[audio] input callback reported status")
                runtime_flags["input_status_seen"] = False
            if runtime_flags["output_status_seen"]:
                print("[audio] output callback reported status")
                runtime_flags["output_status_seen"] = False
            if not input_stream.active or not output_stream.active:
                break
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("[stop] keyboard interrupt")
    except Exception as exc:
        print(f"[audio-runtime-error] {exc}\n{traceback.format_exc()}")
    finally:
        cleanup(input_stream, output_stream)


if __name__ == "__main__":
    main()
