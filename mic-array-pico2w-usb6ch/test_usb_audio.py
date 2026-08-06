#!/usr/bin/env python3
"""List and record from the INMP441 USB microphone array on Windows.

Requirements:
    pip install sounddevice

Examples:
    python test_usb_audio.py --list
    python test_usb_audio.py --info
    python test_usb_audio.py --record-seconds 5 --output test_capture.wav
    python test_usb_audio.py --device-substr "INMP441" --channels 8 --samplerate 44100
"""

from __future__ import annotations

import argparse
import sys
import threading
import wave
from pathlib import Path

DEFAULT_DEVICE_SUBSTR = "INMP441"
DEFAULT_SAMPLERATE = 44100
DEFAULT_CHANNELS = 8
DEFAULT_SECONDS = 5.0
DEFAULT_OUTPUT = "inmp441_test_capture.wav"
HOSTAPI_PRIORITY = {
    "Windows WASAPI": 0,
    "Windows WDM-KS": 1,
    "Windows DirectSound": 2,
    "MME": 3,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Inspect and record from the INMP441 USB microphone array."
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List all audio devices and exit.",
    )
    parser.add_argument(
        "--info",
        action="store_true",
        help="Print the selected device info and exit.",
    )
    parser.add_argument(
        "--device-index",
        type=int,
        help="Use an explicit sounddevice input index.",
    )
    parser.add_argument(
        "--device-substr",
        default=DEFAULT_DEVICE_SUBSTR,
        help=f"Case-insensitive device name match. Default: {DEFAULT_DEVICE_SUBSTR!r}",
    )
    parser.add_argument(
        "--channels",
        type=int,
        default=DEFAULT_CHANNELS,
        help=f"Requested input channels. Default: {DEFAULT_CHANNELS}",
    )
    parser.add_argument(
        "--samplerate",
        type=int,
        default=DEFAULT_SAMPLERATE,
        help=f"Requested sample rate in Hz. Default: {DEFAULT_SAMPLERATE}",
    )
    parser.add_argument(
        "--record-seconds",
        type=float,
        default=DEFAULT_SECONDS,
        help=f"Duration to record in seconds. Default: {DEFAULT_SECONDS}",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(DEFAULT_OUTPUT),
        help=f"Output WAV path. Default: {DEFAULT_OUTPUT}",
    )
    parser.add_argument(
        "--dtype",
        choices=("int16", "int32"),
        default="int16",
        help=(
            "Host capture format. Firmware UAC2 uses 4 bytes/channel (32-bit PCM slots); "
            "if recording fails or is silent, try int32. Default: int16"
        ),
    )
    parser.add_argument(
        "--exclusive",
        action="store_true",
        help=(
            "Use WASAPI exclusive mode when opening the stream. "
            "Recommended for USB UAC2 devices that do not start reliably in shared mode."
        ),
    )
    return parser.parse_args()


def dtype_sample_width(dtype: str) -> int:
    return 4 if dtype == "int32" else 2


def list_devices() -> None:
    import sounddevice as sd

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


def resolve_device(args: argparse.Namespace) -> tuple[int, dict]:
    import sounddevice as sd

    devices = sd.query_devices()
    hostapis = sd.query_hostapis()

    if args.device_index is not None:
        if args.device_index < 0 or args.device_index >= len(devices):
            raise RuntimeError(
                f"Invalid --device-index {args.device_index}. "
                "Run with --list to inspect available devices."
            )
        device = devices[args.device_index]
        if device["max_input_channels"] <= 0:
            raise RuntimeError(
                f"Device index {args.device_index} is not an input device. "
                "Run with --list to inspect available devices."
            )
        return args.device_index, device

    needle = args.device_substr.lower()
    matches = [
        (index, device)
        for index, device in enumerate(devices)
        if needle in device["name"].lower() and device["max_input_channels"] > 0
    ]

    if not matches:
        raise RuntimeError(
            f"No input device found containing {args.device_substr!r}. "
            "Run with --list to inspect available devices."
        )

    if len(matches) > 1:
        print("Multiple matching input devices found:", file=sys.stderr)
        for index, device in matches:
            hostapi_name = hostapis[device["hostapi"]]["name"]
            print(
                f"  [{index}] {device['name']} "
                f"(hostapi={hostapi_name}, "
                f"max_input_channels={device['max_input_channels']})",
                file=sys.stderr,
            )
        matches.sort(
            key=lambda item: (
                HOSTAPI_PRIORITY.get(hostapis[item[1]["hostapi"]]["name"], 99),
                -item[0],
            )
        )
        chosen_index, chosen_device = matches[0]
        chosen_hostapi = hostapis[chosen_device["hostapi"]]["name"]
        print(
            f"Using preferred match [{chosen_index}] via {chosen_hostapi}.\n",
            file=sys.stderr,
        )
        return chosen_index, chosen_device

    return matches[0]


def print_device_info(device_index: int, device: dict, requested_channels: int) -> None:
    import sounddevice as sd

    hostapi_name = sd.query_hostapis()[device["hostapi"]]["name"]
    print(f"Selected input device: [{device_index}] {device['name']}")
    print(f"Host API: {hostapi_name}")
    print(f"Reported max input channels: {device['max_input_channels']}")
    print(f"Default sample rate: {device['default_samplerate']}")

    if requested_channels > device["max_input_channels"]:
        print(
            f"Warning: requested {requested_channels} channels, but device reports "
            f"only {device['max_input_channels']}.",
            file=sys.stderr,
        )


def resolve_wdmks_fallback_device(
    preferred_name: str, requested_channels: int
) -> tuple[int, dict] | None:
    import sounddevice as sd

    devices = sd.query_devices()
    hostapis = sd.query_hostapis()
    target_name = preferred_name.lower()

    candidates: list[tuple[int, dict]] = []
    for index, device in enumerate(devices):
        hostapi_name = hostapis[device["hostapi"]]["name"]
        if hostapi_name != "Windows WDM-KS":
            continue
        if device["max_input_channels"] < requested_channels:
            continue
        name_l = device["name"].lower()
        if target_name in name_l or name_l in target_name or "inmp441" in name_l:
            candidates.append((index, device))

    if not candidates:
        return None

    # Prefer the highest index; on Windows this is often the current active instance.
    candidates.sort(key=lambda item: item[0], reverse=True)
    return candidates[0]


def write_wav(
    path: Path, raw_bytes: bytes, samplerate: int, channels: int, sample_width_bytes: int
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav_file:
        wav_file.setnchannels(channels)
        wav_file.setsampwidth(sample_width_bytes)
        wav_file.setframerate(samplerate)
        wav_file.writeframes(raw_bytes)


def record_wav(
    device_index: int,
    samplerate: int,
    channels: int,
    seconds: float,
    output_path: Path,
    dtype: str,
    exclusive: bool = False,
) -> None:
    import sounddevice as sd

    if seconds <= 0:
        raise ValueError("--record-seconds must be greater than zero.")

    sw = dtype_sample_width(dtype)
    device_info = sd.query_devices(device_index)
    hostapi_name = sd.query_hostapis()[device_info["hostapi"]]["name"]
    extra_settings = None
    if exclusive:
        if hostapi_name != "Windows WASAPI":
            raise RuntimeError(
                f"--exclusive requires a Windows WASAPI device, got host API {hostapi_name!r}. "
                "Pick the WASAPI index from --list or omit --exclusive."
            )
        extra_settings = sd.WasapiSettings(exclusive=True)

    sd.check_input_settings(
        device=device_index,
        samplerate=samplerate,
        channels=channels,
        dtype=dtype,
        extra_settings=extra_settings,
    )

    chunks: list[bytes] = []
    bytes_per_frame = channels * sw
    target_frames = int(seconds * samplerate)
    captured_frames = 0
    done = threading.Event()
    timeout_seconds = max(seconds + 2.0, 5.0)

    def callback(indata, frames, time_info, status) -> None:
        nonlocal captured_frames
        del time_info

        if status:
            print(f"sounddevice status: {status}", file=sys.stderr)

        chunk = bytes(indata)
        chunks.append(chunk)
        captured_frames += frames

        if captured_frames >= target_frames:
            done.set()

    print(
        f"Recording {seconds:.1f}s from device [{device_index}] at "
        f"{samplerate} Hz, {channels} channels..."
    )

    with sd.RawInputStream(
        samplerate=samplerate,
        device=device_index,
        channels=channels,
        dtype=dtype,
        blocksize=512,
        callback=callback,
        extra_settings=extra_settings,
    ):
        if not done.wait(timeout_seconds):
            raise RuntimeError(
                "Timed out waiting for audio buffers from the selected device. "
                "Try another --device-index or verify the app has microphone access."
            )

    raw_bytes = b"".join(chunks)
    frames_per_channel = len(raw_bytes) // bytes_per_frame if bytes_per_frame else 0

    write_wav(output_path, raw_bytes, samplerate, channels, sw)

    print(f"Saved WAV: {output_path}")
    print(f"Captured frames per channel: {frames_per_channel}")
    print(
        "Next step: open the WAV in Audacity or a DAW and verify how many "
        "channels contain real signal."
    )


def main() -> int:
    args = parse_args()

    if args.list:
        list_devices()
        return 0

    try:
        device_index, device = resolve_device(args)
        print_device_info(device_index, device, args.channels)

        if args.info:
            return 0

        try:
            record_wav(
                device_index=device_index,
                samplerate=args.samplerate,
                channels=args.channels,
                seconds=args.record_seconds,
                output_path=args.output,
                dtype=args.dtype,
                exclusive=args.exclusive,
            )
            return 0
        except Exception as first_exc:
            if args.exclusive and "Invalid sample rate" in str(first_exc):
                fallback = resolve_wdmks_fallback_device(device["name"], args.channels)
                if fallback is not None:
                    fb_index, fb_device = fallback
                    print(
                        "WASAPI exclusive rejected the requested format. "
                        "Retrying with matching WDM-KS input device.",
                        file=sys.stderr,
                    )
                    print_device_info(fb_index, fb_device, args.channels)
                    record_wav(
                        device_index=fb_index,
                        samplerate=args.samplerate,
                        channels=args.channels,
                        seconds=args.record_seconds,
                        output_path=args.output,
                        dtype=args.dtype,
                        exclusive=False,
                    )
                    return 0
            raise
    except Exception as exc:  # pragma: no cover - CLI error handling
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
