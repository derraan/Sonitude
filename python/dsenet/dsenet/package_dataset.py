from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path


def _load_manifest(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> None:
    parser = argparse.ArgumentParser(description="Package DSENet synthetic dataset directory with split indexes.")
    parser.add_argument("--dataset-root", type=Path, required=True)
    parser.add_argument("--name", type=str, default="dsenet_librispeech_synth")
    args = parser.parse_args()

    root = args.dataset_root
    splits = {}
    total = 0
    for split in ("train", "val", "test"):
        mpath = root / split / "manifest.json"
        if not mpath.exists():
            continue
        payload = _load_manifest(mpath)
        samples = payload.get("samples", [])
        total += len(samples)
        splits[split] = {
            "count": len(samples),
            "manifest": str((Path(split) / "manifest.json").as_posix()),
        }

    package = {
        "name": args.name,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "format": "dsenet_synth_v1",
        "splits": splits,
        "total_samples": total,
        "notes": [
            "Synthetic data generated using paper-like distributions from DSENet 2023.",
            "Source speech comes from local LibriSpeech files.",
            "Audio tensors are stored as .npy files (mixture_npy, target_npy).",
        ],
    }
    (root / "dataset_package.json").write_text(json.dumps(package, indent=2), encoding="utf-8")
    (root / "README.md").write_text(
        "\n".join(
            [
                f"# {args.name}",
                "",
                "Packaged DSENet synthetic dataset.",
                "",
                "## Layout",
                "- `train/manifest.json`",
                "- `val/manifest.json`",
                "- `test/manifest.json`",
                "- `dataset_package.json`",
                "",
                "Each split entry stores `mixture_npy` and `target_npy` relative file names.",
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    print("Packaged:", root)
    print("Total samples:", total)


if __name__ == "__main__":
    main()
