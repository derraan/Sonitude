"""Discover runtime YAML and calibration YAML files for the test-bench pickers."""

from __future__ import annotations

from pathlib import Path

from app.config_reader import DEFAULT_CONFIG_PATH
from app.storage.result_store import DEFAULT_DATA_ROOT

_SKIP_NAME_PREFIXES = ("geometry_",)
_SKIP_NAME_FRAGMENTS = ("array_ir_manifest",)


def _is_skipped_name(path: Path) -> bool:
    name = path.name.lower()
    if any(name.startswith(prefix) for prefix in _SKIP_NAME_PREFIXES):
        return True
    return any(fragment in name for fragment in _SKIP_NAME_FRAGMENTS)


def _looks_like_calibration(path: Path, text: str) -> bool:
    name = path.name.lower()
    if name.startswith("calibration") or "calibration_" in name:
        return True
    return "gain_linear" in text and "polarity" in text and "capture:" not in text


def _looks_like_runtime(path: Path, text: str) -> bool:
    if path.resolve() == DEFAULT_CONFIG_PATH.resolve():
        return True
    if path.name.lower() in {"default.yaml", "runtime_config_overlay.yaml", "runtime_config.yaml"}:
        return True
    return "capture:" in text and "calibration_path:" in text


def _read_head(path: Path, limit: int = 4000) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="ignore")[:limit]
    except OSError:
        return ""


def iter_yaml_files(roots: list[Path]) -> list[Path]:
    found: list[Path] = []
    seen: set[Path] = set()
    for root in roots:
        if not root.exists():
            continue
        if root.is_file():
            candidates = [root]
        else:
            candidates = list(root.rglob("*.yaml")) + list(root.rglob("*.yml"))
        for path in candidates:
            if not path.is_file():
                continue
            resolved = path.resolve()
            if resolved in seen or _is_skipped_name(path):
                continue
            if path.name.startswith("session_"):
                continue
            seen.add(resolved)
            found.append(resolved)
    return found


def catalog_roots(*, config_dir: Path | None = None, data_root: Path | None = None) -> tuple[Path, Path]:
    config = Path(config_dir) if config_dir is not None else DEFAULT_CONFIG_PATH.parent
    data = Path(data_root) if data_root is not None else DEFAULT_DATA_ROOT
    return config, data / "calibration"


def list_runtime_yaml_files(*, config_dir: Path | None = None, data_root: Path | None = None) -> list[Path]:
    config, cal_data = catalog_roots(config_dir=config_dir, data_root=data_root)
    out: list[Path] = []
    for path in iter_yaml_files([config, cal_data]):
        text = _read_head(path)
        if _looks_like_runtime(path, text) and not _looks_like_calibration(path, text):
            out.append(path)
    return sorted(out, key=lambda p: (p.name.lower(), str(p).lower()))


def list_calibration_yaml_files(*, config_dir: Path | None = None, data_root: Path | None = None) -> list[Path]:
    config, cal_data = catalog_roots(config_dir=config_dir, data_root=data_root)
    out: list[Path] = []
    for path in iter_yaml_files([config, cal_data]):
        text = _read_head(path)
        if _looks_like_calibration(path, text):
            out.append(path)
    return sorted(out, key=lambda p: (p.name.lower(), str(p).lower()))


def resolve_sidecar(runtime_yaml: Path, relative_or_abs: str) -> Path:
    candidate = Path(relative_or_abs)
    if candidate.is_absolute():
        return candidate
    return (runtime_yaml.parent / candidate).resolve()


def calibration_path_from_runtime(runtime_yaml: Path) -> Path | None:
    from app.config_reader import read_runtime_config_summary

    try:
        summary = read_runtime_config_summary(runtime_yaml)
    except (OSError, KeyError, TypeError, ValueError):
        return None
    if not summary.calibration_path:
        return None
    path = resolve_sidecar(runtime_yaml, summary.calibration_path)
    return path if path.is_file() else None
