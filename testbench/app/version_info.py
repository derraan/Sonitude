"""Reads the Sonitude algorithm's version from the C++ project's CMakeLists.txt.

Recorded in every test's metadata.json so a result can be tied back to the
exact algorithm version that produced it.
"""

from __future__ import annotations

import re
from pathlib import Path

from app.processing.sonitude_binary_locator import find_repo_root

_VERSION_PATTERN = re.compile(r"VERSION\s+(\d+\.\d+\.\d+)")


def read_algorithm_version() -> str:
    try:
        cmake_path = find_repo_root() / "CMakeLists.txt"
        text = cmake_path.read_text(encoding="utf-8")
        match = _VERSION_PATTERN.search(text)
        return match.group(1) if match else "unknown"
    except Exception:  # noqa: BLE001 - version info is best-effort metadata
        return "unknown"
