"""Compile synchronized array IRs into a Sonitude SMV3 coefficient artifact.

This tool does not control REW or Audacity. It starts from exported impulse
responses plus a JSON/YAML manifest.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf

try:
    from .deconvolve import DirectPathWindowSpec, deconvolve_multichannel, direct_path_window
except ImportError:  # pragma: no cover - direct script execution
    from deconvolve import DirectPathWindowSpec, deconvolve_multichannel, direct_path_window

MAGIC = b"SMV3"
HEADER_SIZE = 128
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MICS = 6
DEFAULT_GEOMETRY_ID = "soundbubble_vertical_v1"
DEFAULT_FFT_SIZE = 128
DEFAULT_HOP_SIZE = 32
DEFAULT_REFERENCE_MIC = 2
DEFAULT_LEFT_EAR_MIC = 0
DEFAULT_RIGHT_EAR_MIC = 5
MIC_ID_RE = re.compile(r"^M([0-5])(?:_|$)")


def fnv1a64(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def dtft(h: np.ndarray, n_fft: int, n_bins: int) -> np.ndarray:
    n = np.arange(h.shape[0], dtype=np.float64)
    k = np.arange(n_bins, dtype=np.float64)
    phase = -2.0 * np.pi * np.outer(k, n) / float(n_fft)
    return np.exp(1j * phase) @ h.astype(np.complex128)


def regularized_rtf(h: np.ndarray, ref: int, eps: float) -> tuple[np.ndarray, np.ndarray]:
    href = h[ref]
    mag2 = np.abs(href) ** 2
    valid = mag2 > (eps * 8.0)
    d = np.zeros_like(h)
    for m in range(h.shape[0]):
        d[m] = (h[m] * np.conj(href)) / (mag2 + eps)
    d[ref] = np.where(valid, 1.0 + 0.0j, d[ref])
    return d, valid


def loaded_mvdr(gamma: np.ndarray, d: np.ndarray, lam: float) -> np.ndarray:
    a = gamma + lam * np.eye(gamma.shape[0], dtype=np.complex128)
    q = np.linalg.solve(a, d)
    denom = np.vdot(d, q)
    if abs(denom) < 1e-18:
        return d / (np.vdot(d, d) + 1e-18)
    return q / denom


def pack_profile(profile: dict) -> bytes:
    dirs = int(profile["direction_count"])
    bins = int(profile["bin_count"])
    mics = int(profile["mic_count"])
    w = np.asarray(profile["weights"], dtype=np.complex64).reshape(dirs, bins, mics)
    d = np.asarray(profile["steering"], dtype=np.complex64).reshape(dirs, bins, mics)
    valid = np.asarray(profile["valid"], dtype=np.uint8).reshape(dirs, bins)
    scale = np.asarray(profile["dominance_scale"], dtype=np.float32).reshape(bins)
    az = np.asarray(profile["azimuth_deg"], dtype=np.float32).reshape(dirs)

    def cpx_bytes(arr: np.ndarray) -> bytes:
        interleaved = np.empty(arr.size * 2, dtype=np.float32)
        interleaved[0::2] = arr.real.reshape(-1)
        interleaved[1::2] = arr.imag.reshape(-1)
        return interleaved.tobytes()

    valid_pad = (valid.size + 3) & ~3
    valid_b = valid.tobytes() + (b"\x00" * (valid_pad - valid.size))
    payload = cpx_bytes(w) + cpx_bytes(d) + valid_b + scale.tobytes() + az.tobytes()
    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    struct.pack_into("<H", header, 4, 1)
    header[6] = 1
    header[7] = 1
    struct.pack_into("<I", header, 8, int(profile["sample_rate_hz"]))
    struct.pack_into("<HH", header, 12, int(profile["fft_size"]), int(profile["hop_size"]))
    header[16] = mics
    header[17] = bins
    struct.pack_into("<H", header, 18, dirs)
    header[20] = int(profile["reference_mic"])
    header[21] = int(profile["left_ear_mic"])
    header[22] = int(profile["right_ear_mic"])
    header[23] = 0
    struct.pack_into("<I", header, 24, len(payload))
    geom = str(profile.get("geometry_id", "synthetic")).encode("ascii")[:31]
    header[32 : 32 + len(geom)] = geom
    conditioning = bytes(profile.get("conditioning_hash_bytes", b"\x00" * 16))
    header[64 : 64 + 16] = conditioning[:16].ljust(16, b"\x00")
    header[96] = 1 if profile.get("synthetic", True) else 0
    header[97] = int(profile.get("noise_model", 0))
    struct.pack_into("<f", header, 100, float(profile.get("reference_gain", 1.0)))
    digest = fnv1a64(payload)
    struct.pack_into("<I", header, 80, digest & 0xFFFFFFFF)
    struct.pack_into("<I", header, 84, digest >> 32)
    return bytes(header) + payload


def _load_yaml(path: Path) -> Any:
    try:
        import yaml  # type: ignore
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise RuntimeError(
            f"YAML file {path} requires PyYAML; use JSON or install PyYAML."
        ) from exc
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def _load_mapping(path: Path) -> dict[str, Any]:
    suffix = path.suffix.lower()
    if suffix == ".json":
        raw = json.loads(path.read_text(encoding="utf-8"))
    elif suffix in {".yaml", ".yml"}:
        raw = _load_yaml(path)
    else:
        raise ValueError(f"Unsupported manifest format for {path}; use .json/.yaml/.yml")
    if not isinstance(raw, dict):
        raise ValueError(f"{path} must contain a top-level object")
    return raw


def _as_int(mapping: dict[str, Any], key: str, *, minimum: int | None = None) -> int:
    value = mapping.get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ValueError(f"Manifest field '{key}' must be numeric")
    out = int(value)
    if float(out) != float(value):
        raise ValueError(f"Manifest field '{key}' must be an integer")
    if minimum is not None and out < minimum:
        raise ValueError(f"Manifest field '{key}' must be >= {minimum}")
    return out


def _as_float(mapping: dict[str, Any], key: str, *, minimum: float | None = None) -> float:
    value = mapping.get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ValueError(f"Manifest field '{key}' must be numeric")
    out = float(value)
    if not math.isfinite(out):
        raise ValueError(f"Manifest field '{key}' must be finite")
    if minimum is not None and out < minimum:
        raise ValueError(f"Manifest field '{key}' must be >= {minimum}")
    return out


def _parse_vec6(value: Any, key: str) -> np.ndarray:
    if value is None:
        return np.ones(MICS, dtype=np.float64)
    if not isinstance(value, list) or len(value) != MICS:
        raise ValueError(f"Manifest field '{key}' must be a list of length 6")
    vec = np.asarray(value, dtype=np.float64)
    if not np.all(np.isfinite(vec)):
        raise ValueError(f"Manifest field '{key}' must contain finite values")
    return vec


def load_manifest(path: str | Path) -> dict[str, Any]:
    manifest_path = Path(path)
    raw = _load_mapping(manifest_path)
    schema_version = _as_int(raw, "schema_version", minimum=1)
    if schema_version not in {1, 2}:
        raise ValueError(f"Unsupported manifest schema_version: {schema_version}")
    layout = str(raw.get("layout", "")).strip()
    if layout not in {"multichannel", "per_mic"}:
        raise ValueError("Manifest field 'layout' must be 'multichannel' or 'per_mic'")
    directions = raw.get("directions")
    if not isinstance(directions, list) or not directions:
        raise ValueError("Manifest field 'directions' must be a non-empty list")

    manifest: dict[str, Any] = {
        "schema_version": schema_version,
        "layout": layout,
        "sample_rate_hz": _as_int(raw, "sample_rate_hz", minimum=1),
        "geometry_id": str(raw.get("geometry_id", DEFAULT_GEOMETRY_ID)),
        "fft_size": int(raw.get("fft_size", DEFAULT_FFT_SIZE)),
        "hop_size": int(raw.get("hop_size", DEFAULT_HOP_SIZE)),
        "reference_mic": int(raw.get("reference_mic", DEFAULT_REFERENCE_MIC)),
        "left_ear_mic": int(raw.get("left_ear_mic", DEFAULT_LEFT_EAR_MIC)),
        "right_ear_mic": int(raw.get("right_ear_mic", DEFAULT_RIGHT_EAR_MIC)),
        "self_noise": float(raw.get("self_noise", 1e-3)),
        "max_weight_norm": float(raw.get("max_weight_norm", 4.0)),
        "exclude_look_from_noise": bool(raw.get("exclude_look_from_noise", True)),
        "calibration_yaml": raw.get("calibration_yaml"),
        "gains": raw.get("gains"),
        "polarity": raw.get("polarity"),
        "directions": directions,
        "input": str(raw.get("input", "ir")).strip().lower(),
        "stimulus": raw.get("stimulus"),
        "window": raw.get("window"),
        "deconv_eps_rel": float(raw.get("deconv_eps_rel", 1.0e-6)),
    }
    if manifest["input"] not in {"ir", "sweep"}:
        raise ValueError("Manifest field 'input' must be 'ir' or 'sweep'")
    if manifest["input"] == "sweep":
        stimulus = manifest.get("stimulus")
        if not isinstance(stimulus, str) or not stimulus.strip():
            raise ValueError("Manifest field 'stimulus' is required for input='sweep'")
    for key in ("reference_mic", "left_ear_mic", "right_ear_mic"):
        idx = int(manifest[key])
        if idx < 0 or idx >= MICS:
            raise ValueError(f"Manifest field '{key}' must be in [0,5]")
    if manifest["fft_size"] <= 0 or manifest["hop_size"] <= 0:
        raise ValueError("fft_size and hop_size must be positive")
    if manifest["self_noise"] < 0.0 or manifest["max_weight_norm"] <= 0.0:
        raise ValueError("self_noise must be >= 0 and max_weight_norm must be > 0")
    return manifest


def _parse_window_spec(raw: Any) -> DirectPathWindowSpec:
    if raw is None:
        return DirectPathWindowSpec()
    if not isinstance(raw, dict):
        raise ValueError("Manifest field 'window' must be an object")
    pre_samples = int(raw.get("pre_samples", 8))
    length_samples = int(raw.get("length_samples", 256))
    taper = str(raw.get("taper", "tukey")).strip().lower()
    tukey_alpha = float(raw.get("tukey_alpha", 0.25))
    return DirectPathWindowSpec(
        pre_samples=pre_samples,
        length_samples=length_samples,
        taper=taper,
        tukey_alpha=tukey_alpha,
    )


def _resolve_path(base_dir: Path, raw: str) -> Path:
    path = Path(raw)
    if not path.is_absolute():
        path = (base_dir / path).resolve()
    return path


def _load_audio(path: Path) -> tuple[np.ndarray, int]:
    data, sr = sf.read(str(path), dtype="float64", always_2d=True)
    if data.shape[0] == 0:
        raise ValueError(f"IR file is empty: {path}")
    return np.asarray(data, dtype=np.float64), int(sr)


def _parse_direction_sources(manifest: dict[str, Any], base_dir: Path) -> list[dict[str, Any]]:
    sources: list[dict[str, Any]] = []
    seen_az: set[float] = set()
    missing: list[Path] = []
    default_layout = manifest["layout"]
    for i, item in enumerate(manifest["directions"]):
        if not isinstance(item, dict):
            raise ValueError(f"directions[{i}] must be an object")
        if "azimuth_deg" not in item:
            raise ValueError(f"directions[{i}] missing azimuth_deg")
        az = float(item["azimuth_deg"])
        if not math.isfinite(az):
            raise ValueError(f"directions[{i}].azimuth_deg must be finite")
        if az in seen_az:
            raise ValueError(f"Duplicate azimuth_deg in manifest: {az}")
        seen_az.add(az)

        layout = str(item.get("layout", default_layout))
        if layout not in {"multichannel", "per_mic"}:
            raise ValueError(f"directions[{i}].layout must be 'multichannel' or 'per_mic'")
        if layout == "multichannel":
            if "path" not in item:
                raise ValueError(f"directions[{i}] missing path for multichannel layout")
            p = _resolve_path(base_dir, str(item["path"]))
            if not p.exists():
                missing.append(p)
            sources.append({"azimuth_deg": az, "layout": layout, "paths": [p]})
        else:
            channels = item.get("channels")
            if not isinstance(channels, list) or len(channels) != MICS:
                raise ValueError(f"directions[{i}].channels must be a list of 6 paths")
            resolved = [_resolve_path(base_dir, str(c)) for c in channels]
            for p in resolved:
                if not p.exists():
                    missing.append(p)
            sources.append({"azimuth_deg": az, "layout": layout, "paths": resolved})

    if missing:
        listed = "\n".join(sorted(str(p) for p in missing))
        raise ValueError(f"Manifest references missing files:\n{listed}")
    return sources


def _load_stimulus(path: Path, expected_sr: int) -> np.ndarray:
    wav, sr = _load_audio(path)
    if sr != expected_sr:
        raise ValueError(f"Stimulus sample-rate mismatch for {path}: {sr} != {expected_sr}")
    return wav[:, 0].copy()


def load_ir_cube(
    manifest: dict[str, Any], base_dir: str | Path
) -> tuple[np.ndarray, list[float], int, list[dict[str, Any]]]:
    src = _parse_direction_sources(manifest, Path(base_dir))
    expected_sr = int(manifest["sample_rate_hz"])
    direction_waves: list[np.ndarray] = []
    azimuths: list[float] = []
    report_rows: list[dict[str, Any]] = []
    max_len = 0
    input_kind = str(manifest.get("input", "ir"))
    window_spec = _parse_window_spec(manifest.get("window"))
    stimulus = None
    if input_kind == "sweep":
        stimulus = _load_stimulus(_resolve_path(Path(base_dir), str(manifest["stimulus"])), expected_sr)
    for item in src:
        if item["layout"] == "multichannel":
            wav, sr = _load_audio(item["paths"][0])
            if sr != expected_sr:
                raise ValueError(f"Sample-rate mismatch for {item['paths'][0]}: {sr} != {expected_sr}")
            if wav.shape[1] != MICS:
                raise ValueError(f"{item['paths'][0]} must have exactly 6 channels")
            per_dir_raw = wav.T
        else:
            chans: list[np.ndarray] = []
            for p in item["paths"]:
                wav, sr = _load_audio(p)
                if sr != expected_sr:
                    raise ValueError(f"Sample-rate mismatch for {p}: {sr} != {expected_sr}")
                if wav.shape[1] != 1:
                    raise ValueError(f"{p} must be mono for per_mic layout")
                chans.append(wav[:, 0])
            per_dir_raw = np.stack(chans, axis=0)
        if input_kind == "sweep":
            assert stimulus is not None
            irs = deconvolve_multichannel(
                per_dir_raw.T,
                stimulus,
                eps_rel=float(manifest.get("deconv_eps_rel", 1.0e-6)),
            )
            windowed = direct_path_window(irs, window_spec)
            per_dir = windowed.windowed
            report_rows.append(
                {
                    "azimuth_deg": float(item["azimuth_deg"]),
                    "window_start_sample": int(windowed.start_sample),
                    "window_stop_sample": int(windowed.stop_sample),
                    "direct_arrival_samples": [int(v) for v in windowed.arrival_samples],
                    "window": {
                        "pre_samples": int(window_spec.pre_samples),
                        "length_samples": int(window_spec.length_samples),
                        "taper": str(window_spec.taper),
                        "tukey_alpha": float(window_spec.tukey_alpha),
                    },
                }
            )
        else:
            per_dir = per_dir_raw
        max_len = max(max_len, int(per_dir.shape[1]))
        direction_waves.append(per_dir)
        azimuths.append(float(item["azimuth_deg"]))

    irs = np.zeros((len(direction_waves), MICS, max_len), dtype=np.float64)
    for di, arr in enumerate(direction_waves):
        irs[di, :, : arr.shape[1]] = arr
    return irs, azimuths, expected_sr, report_rows


def _m3_channel_index(ch: dict[str, Any], fallback_index: int) -> int:
    raw_id = ch.get("id")
    if isinstance(raw_id, str):
        match = MIC_ID_RE.match(raw_id)
        if match:
            return int(match.group(1))
    return fallback_index


def _load_m3_conditioner(path: Path) -> tuple[np.ndarray, np.ndarray]:
    raw = _load_mapping(path)
    channels = raw.get("channels")
    if not isinstance(channels, list):
        raise ValueError(f"{path} missing channels list")
    gains = np.ones(MICS, dtype=np.float64)
    polarity = np.ones(MICS, dtype=np.float64)
    seen: set[int] = set()
    for i, ch_any in enumerate(channels):
        if not isinstance(ch_any, dict):
            continue
        idx = _m3_channel_index(ch_any, i)
        if idx < 0 or idx >= MICS or idx in seen:
            continue
        seen.add(idx)
        if "gain_linear" in ch_any:
            gains[idx] = float(ch_any["gain_linear"])
        if "polarity" in ch_any:
            polarity[idx] = float(ch_any["polarity"])
    if len(seen) != MICS:
        raise ValueError(f"{path} must define 6 unique channels for gain/polarity import")
    if not np.all(np.isfinite(gains)) or not np.all(np.isfinite(polarity)):
        raise ValueError(f"{path} contains non-finite gain/polarity values")
    return gains, polarity


def load_conditioner(manifest: dict[str, Any], base_dir: str | Path) -> tuple[np.ndarray, np.ndarray]:
    gains = np.ones(MICS, dtype=np.float64)
    polarity = np.ones(MICS, dtype=np.float64)
    calib = manifest.get("calibration_yaml")
    if isinstance(calib, str) and calib.strip():
        gains, polarity = _load_m3_conditioner(_resolve_path(Path(base_dir), calib))

    g_override = manifest.get("gains")
    if g_override is not None:
        gains = _parse_vec6(g_override, "gains")
    p_override = manifest.get("polarity")
    if p_override is not None:
        polarity = _parse_vec6(p_override, "polarity")
    return gains, polarity


def conditioning_hash(gains: np.ndarray, polarity: np.ndarray) -> bytes:
    vec = np.concatenate(
        (
            np.asarray(gains, dtype=np.float32).reshape(-1),
            np.asarray(polarity, dtype=np.float32).reshape(-1),
        )
    )
    h = fnv1a64(vec.tobytes())
    low = h.to_bytes(8, "little", signed=False)
    high = ((h * 0x9E3779B185EBCA87) & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little", signed=False)
    return low + high


def compile_from_irs(
    irs: np.ndarray,
    azimuths: list[float],
    sample_rate_hz: int,
    fft_size: int = 128,
    hop_size: int = 32,
    reference_mic: int = 0,
    left_ear: int = 0,
    right_ear: int = 5,
    gains: np.ndarray | None = None,
    polarity: np.ndarray | None = None,
    max_weight_norm: float = 4.0,
    self_noise: float = 1e-3,
    conditioning_hash_bytes: bytes | None = None,
    exclude_look_from_noise: bool = True,
) -> dict:
    if irs.ndim != 3 or irs.shape[1] != MICS:
        raise ValueError("irs must be [dir, 6, time]")
    n_dir, _, _ = irs.shape
    n_bins = (fft_size // 2) + 1
    gains = np.ones(MICS) if gains is None else np.asarray(gains, dtype=np.float64)
    polarity = np.ones(MICS) if polarity is None else np.asarray(polarity, dtype=np.float64)
    h_all = np.zeros((n_dir, n_bins, MICS), dtype=np.complex128)
    for di in range(n_dir):
        for m in range(MICS):
            h_all[di, :, m] = (gains[m] * polarity[m]) * dtft(irs[di, m], fft_size, n_bins)

    d_all = np.zeros_like(h_all)
    valid = np.zeros((n_dir, n_bins), dtype=np.uint8)
    eps = 1e-8
    for di in range(n_dir):
        d_m, v = regularized_rtf(np.moveaxis(h_all[di], 0, 1), reference_mic, eps)
        d_all[di] = np.moveaxis(d_m, 0, 1)
        valid[di] = v.astype(np.uint8)

    w_all = np.zeros_like(d_all)
    gamma = np.zeros((n_bins, MICS, MICS), dtype=np.complex128)
    for di in range(n_dir):
        # Capon noise model: average outer products of non-look ATFs.
        # Including the look ATF in Γ (previous default) dilutes null depth and
        # is a common reason IR-based MVDR polar plots look nearly omnidirectional.
        if exclude_look_from_noise and n_dir > 1:
            indices = [j for j in range(n_dir) if j != di]
        else:
            indices = list(range(n_dir))
        p = 1.0 / float(len(indices))
        for k in range(n_bins):
            acc = np.zeros((MICS, MICS), dtype=np.complex128)
            for dj in indices:
                h = h_all[dj, k, :][:, None]
                acc += p * (h @ h.conj().T)
            acc += self_noise * np.eye(MICS)
            gamma[k] = 0.5 * (acc + acc.conj().T)

        for k in range(n_bins):
            if not valid[di, k]:
                d = d_all[di, k, :]
                w_all[di, k, :] = d / (np.vdot(d, d) + 1e-18)
                continue
            lam = 1e-4
            w = None
            for _ in range(12):
                w = loaded_mvdr(gamma[k], d_all[di, k, :], lam)
                if np.vdot(w, w).real <= max_weight_norm:
                    break
                lam *= 3.0
            w_all[di, k, :] = w
            unity = np.vdot(d_all[di, k, :], w_all[di, k, :])
            if abs(unity - 1.0) > 1e-3 and abs(unity) > 1e-12:
                w_all[di, k, :] = w_all[di, k, :] / np.conj(unity)

    scale = np.ones(n_bins, dtype=np.float32)
    return {
        "sample_rate_hz": sample_rate_hz,
        "fft_size": fft_size,
        "hop_size": hop_size,
        "mic_count": MICS,
        "bin_count": n_bins,
        "direction_count": n_dir,
        "reference_mic": reference_mic,
        "left_ear_mic": left_ear,
        "right_ear_mic": right_ear,
        "reference_gain": 1.0,
        "synthetic": True,
        "noise_model": 0,
        "geometry_id": "synthetic",
        "azimuth_deg": np.asarray(azimuths, dtype=np.float32),
        "weights": w_all.astype(np.complex64),
        "steering": d_all.astype(np.complex64),
        "valid": valid,
        "dominance_scale": scale,
        "conditioning_hash_bytes": conditioning_hash_bytes or (b"\x00" * 16),
    }


def write_outputs(profile: dict, out_prefix: Path, report_extra: dict[str, Any] | None = None) -> None:
    out_prefix.parent.mkdir(parents=True, exist_ok=True)
    blob = pack_profile(profile)
    (out_prefix.with_suffix(".bin")).write_bytes(blob)
    np.savez(
        out_prefix.with_suffix(".npz"),
        weights=profile["weights"],
        steering=profile["steering"],
        azimuth_deg=profile["azimuth_deg"],
        valid=profile["valid"],
        dominance_scale=profile["dominance_scale"],
    )
    rows = ["dir,az,bin,mic,w_re,w_im,d_re,d_im,valid"]
    w = profile["weights"]
    d = profile["steering"]
    for di, az in enumerate(profile["azimuth_deg"]):
        for k in range(profile["bin_count"]):
            for m in range(profile["mic_count"]):
                rows.append(
                    f"{di},{float(az)},{k},{m},{w[di,k,m].real:.8g},{w[di,k,m].imag:.8g},"
                    f"{d[di,k,m].real:.8g},{d[di,k,m].imag:.8g},{int(profile['valid'][di,k])}"
                )
    (out_prefix.with_suffix(".csv")).write_text("\n".join(rows) + "\n", encoding="utf-8")
    report = {
        "synthetic": bool(profile.get("synthetic", True)),
        "geometry_id": str(profile.get("geometry_id", "synthetic")),
        "directions": int(profile["direction_count"]),
        "bins": int(profile["bin_count"]),
        "bytes": len(blob),
        "noise_model": "angular_atf_plus_documented_self_noise",
        "self_noise_note": "Identity loading is a documented synthetic floor, not a measured sensor noise PSD.",
        "conditioning_hash_hex": bytes(profile.get("conditioning_hash_bytes", b"")).hex(),
    }
    if report_extra:
        report.update(report_extra)
    (out_prefix.with_suffix(".report.json")).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def make_synthetic_impulse_cube(sample_rate_hz: int, delays: list[list[float]]) -> np.ndarray:
    _ = sample_rate_hz
    n = 512
    cube = np.zeros((len(delays), MICS, n), dtype=np.float64)
    t = np.arange(n)
    for di, drow in enumerate(delays):
        for m, delay in enumerate(drow):
            cube[di, m] = np.exp(-0.5 * ((t - (64.0 + delay)) / 2.5) ** 2)
    return cube


def _compile_synthetic(args: argparse.Namespace) -> None:
    delays = [
        [0.0, 0.4, 0.8, 1.2, 1.6, 2.0],
        [0.5, 0.0, 0.4, 0.9, 1.3, 1.8],
    ]
    irs = make_synthetic_impulse_cube(args.sample_rate, delays)
    profile = compile_from_irs(irs, [0.0, 10.0], args.sample_rate)
    write_outputs(profile, Path(args.output_prefix))
    print(f"wrote synthetic profile prefix {args.output_prefix}")


def _compile_manifest(args: argparse.Namespace) -> None:
    manifest_path = Path(args.manifest).resolve()
    manifest = load_manifest(manifest_path)
    irs, azimuths, sample_rate_hz, direction_meta = load_ir_cube(manifest, manifest_path.parent)
    gains, polarity = load_conditioner(manifest, manifest_path.parent)
    cond_hash = conditioning_hash(gains, polarity)
    profile = compile_from_irs(
        irs,
        azimuths,
        sample_rate_hz,
        fft_size=int(manifest["fft_size"]),
        hop_size=int(manifest["hop_size"]),
        reference_mic=int(manifest["reference_mic"]),
        left_ear=int(manifest["left_ear_mic"]),
        right_ear=int(manifest["right_ear_mic"]),
        gains=gains,
        polarity=polarity,
        max_weight_norm=float(manifest["max_weight_norm"]),
        self_noise=float(manifest["self_noise"]),
        conditioning_hash_bytes=cond_hash,
        exclude_look_from_noise=bool(manifest.get("exclude_look_from_noise", True)),
    )
    profile["synthetic"] = False
    profile["geometry_id"] = str(manifest["geometry_id"])
    report_extra = {
        "input_mode": str(manifest.get("input", "ir")),
        "windowing": direction_meta,
    }
    write_outputs(profile, Path(args.output_prefix), report_extra=report_extra)
    print(f"wrote physical profile prefix {args.output_prefix}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Sonitude array calibration compiler")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--synthetic", action="store_true", help="emit a labeled synthetic fixture")
    mode.add_argument("--manifest", type=str, help="path to physical IR import manifest")
    parser.add_argument("--output-prefix", required=True)
    parser.add_argument("--sample-rate", type=int, default=44100)
    args = parser.parse_args(argv)

    if args.synthetic:
        _compile_synthetic(args)
    else:
        _compile_manifest(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
