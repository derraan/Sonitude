from __future__ import annotations

import re
from typing import Any

_PROFILE_RE = re.compile(
    r"^\[profile\]\s+"
    # Optional leading policy fields (current runtime prints these).
    r"(?:mode=\S+\s+)?"
    r"(?:capture_age=[-+0-9.]+ms\s+)?"
    r"(?:capture_gap=\d+\s+)?"
    r"(?:capture_drained=\d+\s+)?"
    # Stage timings (these are the stable contract we parse).
    r"capture->frame=(?P<capture_to_frame_ms>[-+0-9.]+)ms\s+"
    r"dc=(?P<dc_ms>[-+0-9.]+)ms\s+"
    r"level-valid=(?P<level_valid_ms>[-+0-9.]+)ms\s+"
    r"frame->infer=(?P<frame_to_infer_ms>[-+0-9.]+)ms\s+"
    # Inference timing differs across revisions:
    # - Some prints: infer=...ms
    # - Current prints: infer_mean=...ms  infer_total=...ms  steps=...
    r"(?:"
    r"(?:infer_mean=[-+0-9.]+)ms\s+"
    r"infer_total=(?P<infer_ms_total>[-+0-9.]+)ms\s+"
    r"(?:steps=\d+\s+)?\s*"
    r"|"
    r"infer=(?P<infer_ms_direct>[-+0-9.]+)ms\s+"
    r")"
    r"post\+out=(?P<post_out_ms>[-+0-9.]+)ms\s+"
    r"e2e=(?P<e2e_ms>[-+0-9.]+)ms\s+"
    r"drift=(?P<drift_ppm>[-+0-9.]+)ppm\s+"
    r"q=(?P<capture_queue_fill>\d+)/(?:\s*)?(?P<capture_queue_capacity>\d+)\s+"
    r"drops_oldest=(?P<drops_oldest>\d+)\s+"
    r"out_ahead=(?P<out_ahead_ms>[-+0-9.]+)ms\s+"
    r"dropped=(?P<out_dropped_frames>\d+)\s+"
    r"underrun_frames=(?P<underrun_frames>\d+)\s+"
    r"model_ring=(?P<model_ring_ms>[-+0-9.]+)ms\s+"
    r"drops=(?P<model_ring_drops>\d+)\s+"
    r"resets=(?P<model_ring_resets>\d+)"
)

_FLOAT_KEYS = {
    "capture_to_frame_ms",
    "dc_ms",
    "level_valid_ms",
    "frame_to_infer_ms",
    "post_out_ms",
    "e2e_ms",
    "drift_ppm",
    "out_ahead_ms",
    "model_ring_ms",
}

_INT_KEYS = {
    "capture_queue_fill",
    "capture_queue_capacity",
    "drops_oldest",
    "out_dropped_frames",
    "underrun_frames",
    "model_ring_drops",
    "model_ring_resets",
}


def parse_profile_line(line: str) -> dict[str, Any] | None:
    """Parse one `[profile] ...` line from edge/inference_pipeline.py.

    Returns None when the line is not a profile line or does not match the
    current expected format.
    """
    match = _PROFILE_RE.search(line.strip())
    if not match:
        return None

    parsed: dict[str, Any] = {}
    groups = match.groupdict()

    # Unify inference time key across runtime revisions.
    infer = groups.get("infer_ms_total") or groups.get("infer_ms_direct")
    if infer is None:
        return None
    parsed["infer_ms"] = float(infer)

    for key in _FLOAT_KEYS:
        parsed[key] = float(groups[key])
    for key in _INT_KEYS:
        parsed[key] = int(groups[key])
    return parsed

