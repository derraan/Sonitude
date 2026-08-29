#!/usr/bin/env python3
"""
Download SADIE II source SOFA into a local cache for offline coefficient prep.

This script intentionally stores large source datasets outside git-tracked paths.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import urllib.request


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True, help="Direct URL to the SADIE II subject .sofa file")
    parser.add_argument("--output", required=True, help="Output .sofa path in local cache")
    parser.add_argument("--sha256", default="", help="Optional expected SHA-256 checksum")
    args = parser.parse_args()

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {args.url}")
    urllib.request.urlretrieve(args.url, str(output))
    actual = sha256_file(output)
    print(f"Wrote {output} sha256={actual}")
    if args.sha256 and actual.lower() != args.sha256.lower():
        raise SystemExit("Checksum mismatch")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
