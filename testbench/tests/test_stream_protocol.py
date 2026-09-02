"""Protocol v3 unit tests. Fail on nearby-but-wrong properties (wrong seq, old size)."""

from __future__ import annotations

import pytest

from app.processing.protocol import (
    INPUT_HEADER,
    INPUT_MAGIC,
    MSG_AUDIO_BLOCK,
    OUTPUT_HEADER,
    OUTPUT_MAGIC,
    PROTOCOL_VERSION,
    ProtocolError,
    assert_response_sequence,
    pack_input_header,
    unpack_input_header,
    unpack_output_header,
)


def test_input_header_is_76_bytes_not_legacy_48() -> None:
    assert INPUT_HEADER.size == 76
    assert INPUT_HEADER.size != 48


def test_output_header_is_24_bytes_not_legacy_8() -> None:
    assert OUTPUT_HEADER.size == 24
    assert OUTPUT_HEADER.size != 8


def test_input_header_pack_unpack_round_trip() -> None:
    packed = pack_input_header(
        sequence=7,
        frame_count=128,
        payload_length=128 * 6 * 4,
        azimuth_deg=45.0,
        elevation_deg=5.0,
        directivity_blend_deg=30.0,
        suppression_focus_active=True,
        binaural_enabled=True,
        binaural_backend=1,
        suppression_ambient_floor_linear=0.2,
        suppression_fade_ms=80.0,
        suppression_activity_threshold=0.02,
        suppression_confidence_threshold=0.5,
        suppression_envelope_attack_coeff=0.4,
        suppression_envelope_release_coeff=0.008,
        suppression_confidence=0.95,
    )
    header = unpack_input_header(packed)
    assert header.magic == INPUT_MAGIC
    assert header.protocol_version == PROTOCOL_VERSION
    assert header.sequence == 7
    assert header.frame_count == 128
    assert header.azimuth_deg == pytest.approx(45.0)
    assert header.directivity_blend_deg == pytest.approx(30.0)
    assert header.suppression_ambient_floor_linear == pytest.approx(0.2)
    assert header.suppression_fade_ms == pytest.approx(80.0)
    assert header.suppression_confidence == pytest.approx(0.95)


def test_invalid_magic_is_rejected() -> None:
    packed = OUTPUT_HEADER.pack(0xDEADBEEF, PROTOCOL_VERSION, MSG_AUDIO_BLOCK, 0, 1, 0, 8)
    with pytest.raises(ProtocolError, match="invalid magic"):
        unpack_output_header(packed)


def test_unsupported_version_is_rejected() -> None:
    packed = OUTPUT_HEADER.pack(OUTPUT_MAGIC, 99, MSG_AUDIO_BLOCK, 0, 1, 0, 8)
    with pytest.raises(ProtocolError, match="unsupported protocol version"):
        unpack_output_header(packed)


def test_truncated_message_is_rejected() -> None:
    with pytest.raises(ProtocolError, match="truncated"):
        unpack_output_header(b"\x00\x01\x02\x03")


def test_invalid_payload_length_is_rejected() -> None:
    packed = OUTPUT_HEADER.pack(OUTPUT_MAGIC, PROTOCOL_VERSION, MSG_AUDIO_BLOCK, 1, 2, 0, 3)
    with pytest.raises(ProtocolError, match="invalid payload length"):
        unpack_output_header(packed)


def test_invalid_frame_count_is_rejected() -> None:
    packed = OUTPUT_HEADER.pack(OUTPUT_MAGIC, PROTOCOL_VERSION, MSG_AUDIO_BLOCK, 1, 2_000_000, 0, 8)
    with pytest.raises(ProtocolError, match="invalid frame count"):
        unpack_output_header(packed)


def test_unexpected_sequence_is_rejected() -> None:
    with pytest.raises(ProtocolError, match="skipped/unexpected sequence"):
        assert_response_sequence(actual=9, expected=8)


def test_stale_or_duplicate_sequence_is_rejected() -> None:
    with pytest.raises(ProtocolError, match="stale/duplicate"):
        assert_response_sequence(actual=7, expected=8)


def test_matching_sequence_passes() -> None:
    assert_response_sequence(actual=4, expected=4)
