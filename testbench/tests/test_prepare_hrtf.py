"""prepare_hrtf.py must not relabel HRIR samples as a different sample rate."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "tools" / "hrtf" / "prepare_hrtf.py"


def _load_prepare_hrtf():
    spec = importlib.util.spec_from_file_location("prepare_hrtf", MODULE_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_target_rate_must_match_source_rate() -> None:
    prepare = _load_prepare_hrtf()
    assert prepare.resolve_target_rate(44100, 44100) == 44100
    with pytest.raises(RuntimeError, match="does not match SOFA sample rate"):
        prepare.resolve_target_rate(44100, 48000)
