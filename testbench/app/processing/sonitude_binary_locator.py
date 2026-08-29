"""Locates the pre-built Sonitude C++ CLI tools.

The test bench never reimplements or links against the algorithm; it shells
out to the existing (or, for streaming, newly added) portable CLI tools built
from the repo's own CMakeLists.txt. This module is the single place that
knows how to find those binaries, so the rest of the app just asks for a tool
by name.
"""

from __future__ import annotations

import os
import platform
from pathlib import Path

# Tools this app calls. All are part of sonitude_core's portable (ALSA-free)
# subset and build on every platform CMakeLists.txt supports, including Windows.
KNOWN_TOOLS = ("sonitude_wav_replay", "sonitude_stream_process")

_CANDIDATE_BUILD_DIRS = (
    "build",
    "build/Debug",
    "build/Release",
    "build/RelWithDebInfo",
    "out/build/default-debug",
    "out/build/default-release",
    "cmake-build-debug",
    "cmake-build-release",
)


class MissingBinaryError(RuntimeError):
    """Raised when a required Sonitude CLI tool cannot be located."""


def find_repo_root(start: Path | None = None) -> Path:
    """Walk upward from ``start`` (or this file) to find the repo root.

    Identified by the presence of the top-level CMakeLists.txt that defines
    the ``sonitude`` project.
    """
    current = (start or Path(__file__)).resolve()
    for candidate in (current, *current.parents):
        if (candidate / "CMakeLists.txt").is_file() and (candidate / "src" / "main.cpp").is_file():
            return candidate
    raise MissingBinaryError(
        "Could not locate the Sonitude repository root (no CMakeLists.txt/src/main.cpp found "
        f"above {current})"
    )


def _executable_name(tool_name: str) -> str:
    return f"{tool_name}.exe" if platform.system() == "Windows" else tool_name


def find_binary(tool_name: str, build_dir: str | Path | None = None) -> Path:
    """Find a built CLI tool.

    Search order:
      1. Explicit ``build_dir`` argument, if given.
      2. ``SONITUDE_BUILD_DIR`` environment variable.
      3. Common CMake build directory names under the repo root.
    """
    exe_name = _executable_name(tool_name)
    repo_root = find_repo_root()

    search_dirs: list[Path] = []
    if build_dir is not None:
        search_dirs.append(Path(build_dir))
    env_dir = os.environ.get("SONITUDE_BUILD_DIR")
    if env_dir:
        search_dirs.append(Path(env_dir))
    search_dirs.extend(repo_root / d for d in _CANDIDATE_BUILD_DIRS)

    for base in search_dirs:
        candidate = base / exe_name if base.is_absolute() else repo_root / base / exe_name
        if candidate.is_file():
            return candidate
        # Also check directly inside base (covers absolute/custom build dirs).
        direct = Path(base) / exe_name
        if direct.is_file():
            return direct

    raise MissingBinaryError(
        f"Could not find '{exe_name}'. Build the C++ project first (see testbench/README.md), "
        "or set the SONITUDE_BUILD_DIR environment variable to your CMake build directory. "
        f"Searched: {[str(d) for d in search_dirs]}"
    )
