"""Requested vs resolved suppression state.

AUTO uses the YAML runtime.suppression.enabled value.
ON forces the suppressor on.
OFF forces it off, including when YAML says enabled.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class SuppressionMode(str, Enum):
    AUTO = "auto"
    ON = "on"
    OFF = "off"


@dataclass(frozen=True)
class SuppressionState:
    requested: SuppressionMode
    yaml_enabled: bool
    resolved_enabled: bool

    def as_dict(self) -> dict:
        return {
            "requested": self.requested.value,
            "yaml_enabled": self.yaml_enabled,
            "resolved_enabled": self.resolved_enabled,
        }


def parse_suppression_mode(value: str | SuppressionMode | None) -> SuppressionMode:
    if value is None:
        return SuppressionMode.AUTO
    if isinstance(value, SuppressionMode):
        return value
    normalized = value.strip().lower()
    if normalized in ("enable", "enabled", "true", "1"):
        return SuppressionMode.ON
    if normalized in ("disable", "disabled", "false", "0"):
        return SuppressionMode.OFF
    return SuppressionMode(normalized)


def resolve_suppression(requested: SuppressionMode | str, yaml_enabled: bool) -> SuppressionState:
    mode = parse_suppression_mode(requested)
    if mode is SuppressionMode.ON:
        resolved = True
    elif mode is SuppressionMode.OFF:
        resolved = False
    else:
        resolved = bool(yaml_enabled)
    return SuppressionState(requested=mode, yaml_enabled=bool(yaml_enabled), resolved_enabled=resolved)


def cli_args_for(mode: SuppressionMode) -> list[str]:
    return ["--suppression", mode.value]
