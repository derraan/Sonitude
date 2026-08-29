from __future__ import annotations

import pytest

from app.processing.sonitude_binary_locator import MissingBinaryError, find_binary, find_repo_root


def test_find_repo_root_locates_cmake_project() -> None:
    root = find_repo_root()
    assert (root / "CMakeLists.txt").is_file()
    assert (root / "src" / "main.cpp").is_file()


def test_find_binary_raises_clear_error_when_unbuilt() -> None:
    # No build/ directory is expected to exist in a fresh checkout; this
    # should fail with a message telling the user how to fix it, not a
    # generic FileNotFoundError.
    with pytest.raises(MissingBinaryError, match="Build the C\\+\\+ project"):
        find_binary("sonitude_wav_replay", build_dir="__definitely_not_a_real_build_dir__")
