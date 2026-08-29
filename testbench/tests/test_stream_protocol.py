"""Unit tests for the stream_adapter wire format that don't require the
actual sonitude_stream_process subprocess (see test_integration_stream_process.py
for an end-to-end test that runs when the binary is built).
"""

from __future__ import annotations

from app.processing.stream_adapter import _INPUT_HEADER, _INPUT_MAGIC, _OUTPUT_HEADER, _OUTPUT_MAGIC


def test_input_header_size_matches_cpp_wire_format() -> None:
    # magic(4) + frame_count(4) + azimuth(4) + elevation(4) + width(4) +
    # suppression_flag(1) + 3 pad bytes = 24 bytes, matching the field-by-field
    # reads in src/tools/stream_process.cpp's main loop.
    assert _INPUT_HEADER.size == 24


def test_output_header_size_matches_cpp_wire_format() -> None:
    assert _OUTPUT_HEADER.size == 8


def test_input_header_pack_unpack_round_trip() -> None:
    packed = _INPUT_HEADER.pack(_INPUT_MAGIC, 128, 45.0, 0.0, 30.0, 1)
    magic, frame_count, azimuth, elevation, width, flag = _INPUT_HEADER.unpack(packed)
    assert magic == _INPUT_MAGIC
    assert frame_count == 128
    assert azimuth == 45.0
    assert elevation == 0.0
    assert width == 30.0
    assert flag == 1


def test_output_header_pack_unpack_round_trip() -> None:
    packed = _OUTPUT_HEADER.pack(_OUTPUT_MAGIC, 256)
    magic, frame_count = _OUTPUT_HEADER.unpack(packed)
    assert magic == _OUTPUT_MAGIC
    assert frame_count == 256
