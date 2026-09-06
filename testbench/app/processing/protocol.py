"""Versioned binary framing for sonitude_stream_process.

Must stay in lockstep with src/tools/stream_process.cpp. Fields are packed
little-endian with no implicit C struct padding: Python struct layout is
the source of truth for the on-wire size tests.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

PROTOCOL_VERSION = 4
INPUT_MAGIC = 0x32424253  # "SBB2"
OUTPUT_MAGIC = 0x324F4253  # "SBO2"

MSG_AUDIO_BLOCK = 1
MSG_SHUTDOWN = 2
MSG_ERROR = 3

FLAG_SUPPRESSION_FOCUS = 1 << 0
FLAG_BINAURAL_ENABLED = 1 << 1
FLAG_BINAURAL_FOLLOW_STEERING = 1 << 2

OUT_FLAG_SUPPRESSION_APPLIED = 1 << 0
OUT_FLAG_BINAURAL_APPLIED = 1 << 1
OUT_FLAG_BINAURAL_UNAVAILABLE = 1 << 2
OUT_FLAG_MONO_REFERENCE = 1 << 3

BACKEND_NONE = 0
BACKEND_MONO_REFERENCE = 1
BACKEND_ITD_ILD = 2
BACKEND_COMPACT_HRTF = 3
BACKEND_FULL_HRTF_REFERENCE = 4
BACKEND_ARRAY_DOWNMIX = 5

BACKEND_NAMES = {
    BACKEND_NONE: None,
    BACKEND_MONO_REFERENCE: "mono_reference",
    BACKEND_ITD_ILD: "itd_ild",
    BACKEND_COMPACT_HRTF: "compact_hrtf",
    BACKEND_FULL_HRTF_REFERENCE: "full_hrtf_reference",
    BACKEND_ARRAY_DOWNMIX: "array_downmix",
}

# magic I, version H, type H, seq I, frames I, flags I, payload I,
# az f, el f, blend f, bin_az f, bin_el f, backend B, pad 3x,
# suppressor: ambient_floor f, fade_ms f, activity f, confidence_thresh f,
#             env_attack f, env_release f, confidence f
# spectral/mvdr live tuning: gain_floor_db f, protect_ratio f, noise_overestimate f,
#             tonal_ratio f, noise_rise_ms f, mvdr_max_wn_gain f, mvdr_cov_tau_ms f,
#             mvdr_diag_load f
INPUT_HEADER = struct.Struct("<IHHIIII fffff B3x15f")
# magic I, version H, type H, seq I, frames I, flags I, payload I
OUTPUT_HEADER = struct.Struct("<IHHIIII")

MIC_CHANNELS = 6
MAX_DIRECTIVITY_BLEND_DEG = 180.0


class ProtocolError(RuntimeError):
    """Framing, version, sequence, or payload error."""


@dataclass
class InputBlockHeader:
    magic: int
    protocol_version: int
    message_type: int
    sequence: int
    frame_count: int
    flags: int
    payload_length: int
    azimuth_deg: float
    elevation_deg: float
    directivity_blend_deg: float
    binaural_azimuth_deg: float
    binaural_elevation_deg: float
    binaural_backend: int
    suppression_ambient_floor_linear: float
    suppression_fade_ms: float
    suppression_activity_threshold: float
    suppression_confidence_threshold: float
    suppression_envelope_attack_coeff: float
    suppression_envelope_release_coeff: float
    suppression_confidence: float
    spectral_gain_floor_db: float
    spectral_protect_ratio: float
    spectral_noise_overestimate: float
    spectral_tonal_ratio: float
    spectral_noise_rise_ms: float
    mvdr_max_wn_gain: float
    mvdr_cov_tau_ms: float
    mvdr_diag_load: float


@dataclass
class OutputBlockHeader:
    magic: int
    protocol_version: int
    message_type: int
    sequence: int
    frame_count: int
    flags: int
    payload_length: int


def pack_input_header(
    *,
    sequence: int,
    frame_count: int,
    payload_length: int,
    azimuth_deg: float,
    elevation_deg: float,
    directivity_blend_deg: float,
    suppression_focus_active: bool,
    binaural_enabled: bool = False,
    binaural_follow_steering: bool = False,
    binaural_azimuth_deg: float = 0.0,
    binaural_elevation_deg: float = 0.0,
    binaural_backend: int = BACKEND_NONE,
    suppression_ambient_floor_linear: float = 0.25,
    suppression_fade_ms: float = 120.0,
    suppression_activity_threshold: float = 0.03,
    suppression_confidence_threshold: float = 0.6,
    suppression_envelope_attack_coeff: float = 0.35,
    suppression_envelope_release_coeff: float = 0.01,
    suppression_confidence: float = 1.0,
    spectral_gain_floor_db: float = -12.0,
    spectral_protect_ratio: float = 4.0,
    spectral_noise_overestimate: float = 2.0,
    spectral_tonal_ratio: float = 6.0,
    spectral_noise_rise_ms: float = 480.0,
    mvdr_max_wn_gain: float = 4.0,
    mvdr_cov_tau_ms: float = 80.0,
    mvdr_diag_load: float = 0.08,
    message_type: int = MSG_AUDIO_BLOCK,
) -> bytes:
    flags = 0
    if suppression_focus_active:
        flags |= FLAG_SUPPRESSION_FOCUS
    if binaural_enabled:
        flags |= FLAG_BINAURAL_ENABLED
    if binaural_follow_steering:
        flags |= FLAG_BINAURAL_FOLLOW_STEERING
    blend = max(0.0, min(MAX_DIRECTIVITY_BLEND_DEG, float(directivity_blend_deg)))
    return INPUT_HEADER.pack(
        INPUT_MAGIC,
        PROTOCOL_VERSION,
        message_type,
        sequence,
        frame_count,
        flags,
        payload_length,
        float(azimuth_deg),
        float(elevation_deg),
        blend,
        float(binaural_azimuth_deg),
        float(binaural_elevation_deg),
        binaural_backend,
        float(suppression_ambient_floor_linear),
        float(suppression_fade_ms),
        float(suppression_activity_threshold),
        float(suppression_confidence_threshold),
        float(suppression_envelope_attack_coeff),
        float(suppression_envelope_release_coeff),
        float(suppression_confidence),
        float(spectral_gain_floor_db),
        float(spectral_protect_ratio),
        float(spectral_noise_overestimate),
        float(spectral_tonal_ratio),
        float(spectral_noise_rise_ms),
        float(mvdr_max_wn_gain),
        float(mvdr_cov_tau_ms),
        float(mvdr_diag_load),
    )


def unpack_input_header(data: bytes) -> InputBlockHeader:
    if len(data) < INPUT_HEADER.size:
        raise ProtocolError(f"truncated message: {len(data)} < {INPUT_HEADER.size}")
    if len(data) != INPUT_HEADER.size:
        raise ProtocolError(f"truncated input header: {len(data)} != {INPUT_HEADER.size}")
    fields = INPUT_HEADER.unpack(data)
    header = InputBlockHeader(*fields)
    if header.magic != INPUT_MAGIC:
        raise ProtocolError(f"invalid magic: {header.magic:#x}")
    if header.protocol_version != PROTOCOL_VERSION:
        raise ProtocolError(f"unsupported protocol version: {header.protocol_version}")
    return header


def expected_pcm_bytes(frame_count: int, channels: int) -> int:
    if frame_count < 0:
        raise ProtocolError(f"invalid frame count: {frame_count}")
    return int(frame_count) * int(channels) * 4


def assert_response_sequence(actual: int, expected: int | None) -> None:
    if expected is None:
        raise ProtocolError("unexpected response: no request has been sent")
    if actual != expected:
        kind = "stale/duplicate" if actual < expected else "skipped/unexpected"
        raise ProtocolError(f"{kind} sequence: got {actual}, expected {expected}")


def unpack_output_header(data: bytes) -> OutputBlockHeader:
    if len(data) < OUTPUT_HEADER.size:
        raise ProtocolError(f"truncated message: {len(data)} < {OUTPUT_HEADER.size}")
    if len(data) != OUTPUT_HEADER.size:
        raise ProtocolError(f"truncated output header: {len(data)} != {OUTPUT_HEADER.size}")
    magic, version, message_type, sequence, frame_count, flags, payload_length = OUTPUT_HEADER.unpack(data)
    if magic != OUTPUT_MAGIC:
        raise ProtocolError(f"invalid magic: {magic:#x}")
    if version != PROTOCOL_VERSION:
        raise ProtocolError(f"unsupported protocol version: {version}")
    if message_type not in (MSG_AUDIO_BLOCK, MSG_SHUTDOWN, MSG_ERROR):
        raise ProtocolError(f"unknown message type: {message_type}")
    if frame_count > 1_048_576:
        raise ProtocolError(f"invalid frame count: {frame_count}")
    if payload_length > 1_048_576 * 2 * 4:
        raise ProtocolError(f"invalid payload length: {payload_length}")
    if message_type == MSG_AUDIO_BLOCK:
        expected = expected_pcm_bytes(frame_count, 2)
        if payload_length != expected:
            raise ProtocolError(f"invalid payload length: {payload_length} != {expected}")
    return OutputBlockHeader(
        magic=magic,
        protocol_version=version,
        message_type=message_type,
        sequence=sequence,
        frame_count=frame_count,
        flags=flags,
        payload_length=payload_length,
    )


def backend_id(name: str | None) -> int:
    if not name:
        return BACKEND_NONE
    for key, value in BACKEND_NAMES.items():
        if value == name:
            return key
    raise ProtocolError(f"unknown binaural backend {name!r}")
