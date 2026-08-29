"""Audio input device enumeration for real-time mode, via sounddevice/PortAudio."""

from __future__ import annotations

from dataclasses import dataclass

import sounddevice as sd


@dataclass
class InputDeviceInfo:
    index: int
    name: str
    max_input_channels: int
    default_sample_rate_hz: float


def list_input_devices() -> list[InputDeviceInfo]:
    """List all devices with at least one input channel."""
    devices: list[InputDeviceInfo] = []
    for index, device in enumerate(sd.query_devices()):
        if device.get("max_input_channels", 0) > 0:
            devices.append(
                InputDeviceInfo(
                    index=index,
                    name=device["name"],
                    max_input_channels=device["max_input_channels"],
                    default_sample_rate_hz=device["default_samplerate"],
                )
            )
    return devices


def supports_six_channels(device: InputDeviceInfo) -> bool:
    return device.max_input_channels >= 6
