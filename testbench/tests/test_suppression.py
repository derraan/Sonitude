from __future__ import annotations

import yaml

from app.processing.suppression import SuppressionMode, resolve_suppression


def test_auto_follows_yaml_true() -> None:
    state = resolve_suppression(SuppressionMode.AUTO, yaml_enabled=True)
    assert state.requested is SuppressionMode.AUTO
    assert state.resolved_enabled is True


def test_auto_follows_yaml_false() -> None:
    state = resolve_suppression("auto", yaml_enabled=False)
    assert state.resolved_enabled is False


def test_on_overrides_yaml_false() -> None:
    state = resolve_suppression(SuppressionMode.ON, yaml_enabled=False)
    assert state.requested is SuppressionMode.ON
    assert state.yaml_enabled is False
    assert state.resolved_enabled is True


def test_off_overrides_yaml_true() -> None:
    """The previous OR-only behaviour could not force suppression off."""
    state = resolve_suppression(SuppressionMode.OFF, yaml_enabled=True)
    assert state.yaml_enabled is True
    assert state.resolved_enabled is False


def test_cli_and_yaml_are_stored_separately() -> None:
    state = resolve_suppression("off", yaml_enabled=True)
    payload = state.as_dict()
    assert payload["requested"] == "off"
    assert payload["yaml_enabled"] is True
    assert payload["resolved_enabled"] is False


def test_yaml_true_and_cli_off_are_not_confused_by_or(tmp_path) -> None:
    config = {"suppression": {"enabled": True}}
    path = tmp_path / "cfg.yaml"
    path.write_text(yaml.safe_dump(config), encoding="utf-8")
    loaded = yaml.safe_load(path.read_text(encoding="utf-8"))
    yaml_enabled = bool(loaded["suppression"]["enabled"])
    # Nearby wrong property: `cli_on or yaml_enabled` would still be True.
    cli_off = False
    wrong_or = cli_off or yaml_enabled
    correct = resolve_suppression(SuppressionMode.OFF, yaml_enabled).resolved_enabled
    assert wrong_or is True
    assert correct is False
