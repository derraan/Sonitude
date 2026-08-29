from __future__ import annotations

import pytest

from app.processing.sonitude_binary_locator import MissingBinaryError, find_binary, find_repo_root


def test_find_repo_root_locates_cmake_project() -> None:
    root = find_repo_root()
    assert (root / "CMakeLists.txt").is_file()
    assert (root / "src" / "main.cpp").is_file()


def test_find_binary_raises_clear_error_when_unbuilt(tmp_path, monkeypatch) -> None:
    """Must raise MissingBinaryError (not FileNotFoundError) when nothing is found.

    This must not pass merely because a real build/ directory exists on the
    developer machine or CI; candidate dirs and SONITUDE_BUILD_DIR are stubbed.
    """
    monkeypatch.delenv("SONITUDE_BUILD_DIR", raising=False)
    monkeypatch.setattr(
        "app.processing.sonitude_binary_locator._CANDIDATE_BUILD_DIRS",
        ("__definitely_not_a_real_build_dir__",),
    )
    with pytest.raises(MissingBinaryError, match="Build the C\\+\\+ project"):
        find_binary("sonitude_wav_replay", build_dir=tmp_path / "empty_build")
