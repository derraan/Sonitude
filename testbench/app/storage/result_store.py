"""Reproducible test result storage."""

from __future__ import annotations

import json
import shutil
import uuid
from datetime import datetime, timezone
from pathlib import Path

from app.storage.models import TestPaths

DEFAULT_DATA_ROOT = Path(__file__).resolve().parents[2] / "data"


def new_test_id() -> str:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    return f"TEST_{stamp}_{uuid.uuid4().hex[:6]}"


class ResultStore:
    """Creates and manages the on-disk layout for test results."""

    def __init__(self, data_root: str | Path = DEFAULT_DATA_ROOT) -> None:
        self.data_root = Path(data_root)
        self.results_dir = self.data_root / "results"
        self.results_dir.mkdir(parents=True, exist_ok=True)

    def new_test(self, test_id: str | None = None) -> TestPaths:
        test_id = test_id or new_test_id()
        root = self.results_dir / test_id
        root.mkdir(parents=True, exist_ok=True)
        return TestPaths(
            test_id=test_id,
            root=root,
            input_wav=root / "input.wav",
            processed_wav=root / "processed.wav",
            processed_stereo_wav=root / "processed_stereo.wav",
            beamformed_wav=root / "beamformed.wav",
            suppressed_wav=root / "suppressed.wav",
            raw_preview_wav=root / "raw_preview_stereo.wav",
            residual_beamform_wav=root / "residual_beamform.wav",
            residual_limiter_wav=root / "residual_limiter.wav",
            metadata_json=root / "metadata.json",
            metrics_json=root / "metrics.json",
            steering_script=root / "steering_script.csv",
            binaural_wav=root / "binaural_stereo.wav",
            processed_export=root / "processed_export.wav",
            runtime_config_copy=root / "runtime_config.yaml",
            input_metadata_json=root / "input_metadata.json",
        )

    @staticmethod
    def copy_input(source: str | Path, dest: str | Path) -> None:
        """Copy (never move) the original input into the result directory."""
        shutil.copy2(str(source), str(dest))

    @staticmethod
    def save_metadata(paths: TestPaths, metadata: dict) -> None:
        paths.metadata_json.write_text(json.dumps(metadata, indent=2), encoding="utf-8")

    @staticmethod
    def save_metrics(paths: TestPaths, metrics: dict) -> None:
        paths.metrics_json.write_text(json.dumps(metrics, indent=2), encoding="utf-8")

    def list_tests(self) -> list[str]:
        if not self.results_dir.exists():
            return []
        return sorted(p.name for p in self.results_dir.iterdir() if p.is_dir())

    def load_metadata(self, test_id: str) -> dict:
        path = self.results_dir / test_id / "metadata.json"
        return json.loads(path.read_text(encoding="utf-8"))

    def load_metrics(self, test_id: str) -> dict:
        path = self.results_dir / test_id / "metrics.json"
        return json.loads(path.read_text(encoding="utf-8"))
