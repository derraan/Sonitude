"""Reads the Sonitude algorithm version and git commit where available."""

from __future__ import annotations

import subprocess
from pathlib import Path

from app.processing.sonitude_binary_locator import find_repo_root

_VERSION_PATTERN = __import__("re").compile(r"VERSION\s+(\d+\.\d+\.\d+)")


def read_algorithm_version() -> str:
    try:
        cmake_path = find_repo_root() / "CMakeLists.txt"
        text = cmake_path.read_text(encoding="utf-8")
        match = _VERSION_PATTERN.search(text)
        return match.group(1) if match else "unknown"
    except Exception:  # noqa: BLE001
        return "unknown"


def read_git_commit() -> str | None:
    """Return HEAD SHA, or None if it cannot be determined. Never invent one."""
    try:
        repo = find_repo_root()
        proc = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=False,
            timeout=5,
        )
        if proc.returncode != 0:
            return None
        sha = proc.stdout.strip()
        return sha or None
    except Exception:  # noqa: BLE001
        return None
