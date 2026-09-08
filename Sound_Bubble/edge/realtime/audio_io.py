from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable


@dataclass(frozen=True)
class StreamPair:
    input_stream: Any
    output_stream: Any


def preflight_sounddevice(sd_module: Any, input_device: int | None, input_channels: int, input_sr: int, output_device: int | None, output_channels: int, output_sr: int) -> None:
    sd_module.check_input_settings(
        device=input_device,
        channels=int(input_channels),
        samplerate=int(input_sr),
        dtype="float32",
    )
    sd_module.check_output_settings(
        device=output_device,
        channels=int(output_channels),
        samplerate=int(output_sr),
        dtype="float32",
    )


def create_streams(
    sd_module: Any,
    *,
    input_sr: int,
    input_block: int,
    input_channels: int,
    input_latency: str,
    input_callback: Callable,
    input_device: int | None,
    output_sr: int,
    output_block: int,
    output_channels: int,
    output_latency: str,
    output_callback: Callable,
    output_device: int | None,
) -> StreamPair:
    input_stream = sd_module.InputStream(
        samplerate=int(input_sr),
        blocksize=int(input_block),
        channels=int(input_channels),
        dtype="float32",
        latency=input_latency,
        callback=input_callback,
        device=input_device,
    )
    output_stream = sd_module.OutputStream(
        samplerate=int(output_sr),
        blocksize=int(output_block),
        channels=int(output_channels),
        dtype="float32",
        latency=output_latency,
        callback=output_callback,
        device=output_device,
    )
    return StreamPair(input_stream=input_stream, output_stream=output_stream)

