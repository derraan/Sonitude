#!/usr/bin/env python3
"""Check changed first-party C/C++ files with clang-format 18."""

import os
from pathlib import Path
import shutil
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parent.parent
GIT = os.environ.get("GIT", "git")
SOURCE_ROOTS = (b"src/", b"tests/", b"scripts/", b"include/")
SOURCE_SUFFIXES = (b".c", b".h", b".cpp", b".hpp")


def git_output(*arguments: str, check: bool = True) -> bytes:
    result = subprocess.run(
        [GIT, *arguments],
        cwd=REPO_ROOT,
        check=check,
        stdout=subprocess.PIPE,
    )
    return result.stdout


def default_base() -> str:
    if subprocess.run(
        [GIT, "rev-parse", "--verify", "origin/main^{commit}"],
        cwd=REPO_ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode == 0:
        return git_output("merge-base", "HEAD", "origin/main").decode().strip()
    if subprocess.run(
        [GIT, "rev-parse", "--verify", "HEAD^"],
        cwd=REPO_ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode == 0:
        return git_output("rev-parse", "HEAD^").decode().strip()
    return git_output("rev-parse", "HEAD").decode().strip()


def is_first_party_source(path: bytes) -> bool:
    if not path.startswith(SOURCE_ROOTS) or not path.endswith(SOURCE_SUFFIXES):
        return False
    components = path.split(b"/")
    if any(
        component in (b"_deps", b"generated", b"out", b"mic-array-pico2w-usb6ch")
        or component == b"build"
        or component.startswith(b"build-")
        for component in components
    ):
        return False
    return os.path.isfile(REPO_ROOT.as_posix().encode() + b"/" + path)


def main() -> int:
    if len(sys.argv) > 3 or (len(sys.argv) == 3 and sys.argv[2] != "--fix"):
        print(f"Usage: {sys.argv[0]} [base SHA] [--fix]", file=sys.stderr)
        return 2
    fix = len(sys.argv) == 3

    base_sha = sys.argv[1] if len(sys.argv) > 1 else ""
    if not base_sha or set(base_sha) == {"0"}:
        base_sha = default_base()
    git_output("rev-parse", "--verify", f"{base_sha}^{{commit}}")

    outputs = (
        git_output("diff", "--name-only", "--diff-filter=ACMR", "-z", f"{base_sha}...HEAD"),
        git_output("diff", "--name-only", "--diff-filter=ACMR", "-z"),
        git_output("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"),
        git_output("ls-files", "--others", "--exclude-standard", "-z"),
    )

    files: list[bytes] = []
    seen: set[bytes] = set()
    for output in outputs:
        for path in output.split(b"\0"):
            if path and path not in seen and is_first_party_source(path):
                seen.add(path)
                files.append(path)

    if not files:
        print("No changed first-party C/C++ files to check.")
        return 0

    formatter = os.environ.get("CLANG_FORMAT", "clang-format-18")
    if shutil.which(formatter) is None:
        print(f"Required formatter not found: {formatter}", file=sys.stderr)
        return 2

    action = "Formatting" if fix else "Checking"
    print(f"{action} {len(files)} changed first-party C/C++ file(s) with {formatter}.")
    arguments = [os.fsencode(formatter)]
    if fix:
        arguments.append(b"-i")
    else:
        arguments.extend((b"--dry-run", b"--Werror"))
    subprocess.run([*arguments, b"--", *files], cwd=REPO_ROOT, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
