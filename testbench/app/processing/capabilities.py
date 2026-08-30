"""Query C++ CLI tools for features this build actually implements."""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from app.processing.protocol import PROTOCOL_VERSION
from app.processing.sonitude_binary_locator import find_binary


KNOWN_BINAURAL_BACKENDS = ("array_downmix", "mono_reference", "itd_ild", "compact_hrtf", "full_hrtf_reference")
PREFERRED_BINAURAL_BACKENDS = ("array_downmix", "compact_hrtf", "itd_ild", "full_hrtf_reference", "mono_reference")
KNOWN_SUPPRESSION_BACKENDS = ("off", "conservative", "spectral")
PREFERRED_SUPPRESSION_BACKENDS = ("conservative", "spectral", "off")


@dataclass
class SuppressionCapabilities:
    backends: list[str] = field(default_factory=lambda: ["off", "conservative"])
    default_backend: str = "conservative"
    implementation_status: str = ""
    note: str = ""

    def backend_supported(self, name: str) -> bool:
        return name in self.backends


@dataclass
class BinauralCapabilities:
    available: bool = False
    backends: list[str] = field(default_factory=lambda: ["mono_reference"])
    unavailable_backends: list[str] = field(
        default_factory=lambda: ["itd_ild", "compact_hrtf", "full_hrtf_reference"]
    )
    note: str = "HRTF/ITD DSP is not in this build; mono_reference is L=R duplicate of directional mono."

    def backend_supported(self, name: str) -> bool:
        return self.available and name in self.backends


@dataclass
class ToolCapabilities:
    protocol_version: int = PROTOCOL_VERSION
    suppression_modes: list[str] = field(default_factory=lambda: ["auto", "on", "off"])
    suppression: SuppressionCapabilities = field(default_factory=SuppressionCapabilities)
    taps: list[str] = field(default_factory=lambda: ["beamformed", "suppressed", "processed"])
    binaural: BinauralCapabilities = field(default_factory=BinauralCapabilities)
    queried: bool = False
    binary_path: str | None = None
    raw: dict = field(default_factory=dict)

    def as_dict(self) -> dict:
        return {
            "protocol_version": self.protocol_version,
            "suppression_modes": self.suppression_modes,
            "suppression": {
                "backends": self.suppression.backends,
                "default_backend": self.suppression.default_backend,
                "implementation_status": self.suppression.implementation_status,
                "note": self.suppression.note,
            },
            "taps": self.taps,
            "binaural": {
                "available": self.binaural.available,
                "backends": self.binaural.backends,
                "unavailable_backends": self.binaural.unavailable_backends,
                "note": self.binaural.note,
            },
            "queried": self.queried,
            "binary_path": self.binary_path,
        }


def preferred_suppression_backend(
    available: list[str],
    yaml_backend: str | None = None,
) -> str | None:
    """Pick the GUI default from YAML when supported, else the first preferred backend."""
    if yaml_backend and yaml_backend in available:
        return yaml_backend
    for name in PREFERRED_SUPPRESSION_BACKENDS:
        if name in available:
            return name
    return available[0] if available else None


def preferred_binaural_backend(
    available: list[str],
    yaml_backend: str | None = None,
) -> str | None:
    """Pick the GUI default from YAML when supported, else the preferred backend."""
    if yaml_backend and yaml_backend in available:
        return yaml_backend
    for name in PREFERRED_BINAURAL_BACKENDS:
        if name in available:
            return name
    return available[0] if available else None


def parse_capabilities_json(payload: dict) -> ToolCapabilities:
    suppression_raw = payload.get("suppression", {})
    if isinstance(suppression_raw, dict):
        suppression_modes = list(suppression_raw.get("modes", ["auto", "on", "off"]))
        suppression_backends = list(
            suppression_raw.get("backends", ["off", "conservative"])
        )
        suppression_default = str(suppression_raw.get("default_backend", "conservative"))
        suppression_status = str(suppression_raw.get("implementation_status", ""))
        suppression_note = str(suppression_raw.get("note", ""))
    else:
        suppression_modes = list(payload.get("suppression_modes", ["auto", "on", "off"]))
        suppression_backends = ["off", "conservative"]
        suppression_default = "conservative"
        suppression_status = ""
        suppression_note = ""

    binaural_raw = payload.get("binaural", {})
    backends = list(binaural_raw.get("backends", ["mono_reference"]))
    unavailable = list(
        binaural_raw.get(
            "unavailable_backends",
            [name for name in KNOWN_BINAURAL_BACKENDS if name not in backends],
        )
    )
    return ToolCapabilities(
        protocol_version=int(payload.get("protocol_version", PROTOCOL_VERSION)),
        suppression_modes=suppression_modes,
        suppression=SuppressionCapabilities(
            backends=suppression_backends,
            default_backend=suppression_default,
            implementation_status=suppression_status,
            note=suppression_note,
        ),
        taps=list(payload.get("taps", ["beamformed", "suppressed", "processed"])),
        binaural=BinauralCapabilities(
            available=bool(binaural_raw.get("available", False)),
            backends=backends,
            unavailable_backends=unavailable,
            note=str(binaural_raw.get("note", BinauralCapabilities().note)),
        ),
        queried=True,
        raw=payload,
    )


def query_tool_capabilities(
    tool_name: str,
    *,
    binary_path: str | Path | None = None,
    build_dir: str | Path | None = None,
) -> ToolCapabilities:
    """Run ``tool --capabilities``. Missing/old binaries return a gated default."""
    try:
        binary = Path(binary_path) if binary_path else find_binary(tool_name, build_dir)
    except Exception:
        return ToolCapabilities()
    try:
        proc = subprocess.run(
            [str(binary), "--capabilities"],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ToolCapabilities(binary_path=str(binary))
    if proc.returncode != 0:
        return ToolCapabilities(binary_path=str(binary))
    try:
        payload = json.loads(proc.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return ToolCapabilities(binary_path=str(binary))
    parsed = parse_capabilities_json(payload)
    parsed.binary_path = str(binary)
    return parsed
