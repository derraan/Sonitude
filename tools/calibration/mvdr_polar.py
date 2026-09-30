"""Near-field MVDR polar from the realtime host DSP.

Python here only prepares 6-channel probe WAVs (measured captures, or a
geometry delay simulation) and plots energy. Beamforming is exclusively
``sonitude_wav_replay`` → ``MvdrBeamformer`` (or ``FixedBinauralMvdr`` when
``spatial.backend: fixed_measured``), configured from runtime YAML.

Typical:
  python -m tools.calibration.mvdr_polar --synthetic --look-az 0 --plot out.png
  python -m tools.calibration.mvdr_polar --manifest path.yaml --look-az 0 --plot out.png
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf
import yaml

try:
    from .geometry import (
        REFERENCE_INDEX,
        SPEED_OF_SOUND_MPS,
        geometric_delay_samples_spherical,
        load_geometry,
        unit_vector_from_az_el_deg,
    )
except ImportError:  # pragma: no cover
    from geometry import (  # type: ignore
        REFERENCE_INDEX,
        SPEED_OF_SOUND_MPS,
        geometric_delay_samples_spherical,
        load_geometry,
        unit_vector_from_az_el_deg,
    )

MICS = 6
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
    """Raised when sonitude_wav_replay cannot be located."""


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def find_wav_replay(explicit: str | Path | None = None, build_dir: str | Path | None = None) -> Path:
    exe_name = "sonitude_wav_replay.exe" if platform.system() == "Windows" else "sonitude_wav_replay"
    if explicit is not None:
        path = Path(explicit)
        if not path.is_file():
            raise MissingBinaryError(f"wav_replay binary not found: {path}")
        return path
    root = repo_root()
    search: list[Path] = []
    if build_dir is not None:
        search.append(Path(build_dir))
    env_dir = os.environ.get("SONITUDE_BUILD_DIR")
    if env_dir:
        search.append(Path(env_dir))
    search.extend(root / d for d in _CANDIDATE_BUILD_DIRS)
    for base in search:
        for candidate in (base / exe_name, Path(base) / exe_name):
            if candidate.is_file():
                return candidate
    raise MissingBinaryError(
        f"Could not find '{exe_name}'. Build sonitude_wav_replay first, or pass --binary / SONITUDE_BUILD_DIR."
    )


def load_runtime_yaml(path: Path) -> dict[str, Any]:
    raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise RuntimeError(f"runtime YAML is not a mapping: {path}")
    return raw


def resolve_runtime_path(runtime_yaml: Path, relative: str) -> Path:
    candidate = Path(relative)
    if candidate.is_file():
        return candidate
    return (runtime_yaml.parent / relative).resolve()


def summarize_suppression(azimuths: np.ndarray, pattern_db: np.ndarray, look_az: float) -> dict[str, float]:
    look_i = int(np.argmin(np.abs(((azimuths - look_az + 180) % 360) - 180)))
    look_db = float(pattern_db[look_i])
    peak_i = int(np.argmax(pattern_db))
    mask = np.abs(((azimuths - look_az + 180) % 360) - 180) >= 60.0
    if not np.any(mask):
        off = float(np.min(pattern_db))
    else:
        off = float(np.mean(pattern_db[mask]))
    return {
        "look_az_deg": float(azimuths[look_i]),
        "look_response_db": look_db,
        "peak_az_deg": float(azimuths[peak_i]),
        "peak_response_db": float(pattern_db[peak_i]),
        "mean_offaxis_db": off,
        "suppression_db": look_db - off,
        "min_db": float(np.min(pattern_db)),
        "max_db": float(np.max(pattern_db)),
    }


def polar_plot_coords(azimuths_deg: np.ndarray, pattern_db: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Map head-frame azimuth / look-normalized dB to matplotlib polar coords.

    Convention: nose-forward (0°) at the top, positive azimuth clockwise toward
    listener-right — matching head_frame and set_theta_zero_location('N') with
    clockwise direction. Radius is look-normalized dB shifted so the deepest
    null sits near the origin (larger radius = stronger response).
    """
    order = np.argsort(azimuths_deg)
    az = np.asarray(azimuths_deg, dtype=np.float64)[order]
    pat = np.asarray(pattern_db, dtype=np.float64)[order]
    theta = np.deg2rad(az)
    floor = float(np.min(pat))
    radius = pat - floor + 1.0
    wrap_gap = (float(az[0]) + 360.0) - float(az[-1])
    step = float(np.max(np.diff(az))) if az.size > 1 else 360.0
    if wrap_gap <= step * 1.5 + 1e-6:
        theta = np.concatenate([theta, theta[:1]])
        radius = np.concatenate([radius, radius[:1]])
    return theta, radius


def plot_polar(azimuths_deg: np.ndarray, pattern_db: np.ndarray, out_path: Path, title: str) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    theta, radius = polar_plot_coords(azimuths_deg, pattern_db)
    fig, ax = plt.subplots(subplot_kw={"projection": "polar"}, figsize=(7, 7))
    ax.set_theta_zero_location("N")
    ax.set_theta_direction(-1)
    ax.set_thetagrids(
        [0, 45, 90, 135, 180, 225, 270, 315],
        labels=["0° front", "45°", "90° right", "135°", "180° back", "225°", "270° left", "315°"],
    )
    ax.plot(theta, radius, color="#0b3d5c", linewidth=2.0)
    closed = theta.size > 1 and abs(float(theta[0] - theta[-1])) < 1e-12
    if closed:
        ax.fill(theta, radius, color="#0b3d5c", alpha=0.18)
    ax.set_title(title)
    ax.set_ylim(0, float(np.max(radius)) * 1.05)
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=140)
    plt.close(fig)


def _resolve_path(base_dir: Path, raw: str) -> Path:
    path = Path(raw)
    if not path.is_absolute():
        path = (base_dir / path).resolve()
    return path


def load_probe_recordings(manifest_path: Path) -> list[tuple[float, Path]]:
    """Load 6-channel capture paths per azimuth. Does not deconvolve or beamform."""
    raw = yaml.safe_load(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError(f"manifest is not a mapping: {manifest_path}")
    directions = raw.get("directions")
    if not isinstance(directions, list) or not directions:
        raise ValueError("manifest field 'directions' must be a non-empty list")
    default_layout = str(raw.get("layout", "multichannel")).strip()
    base = manifest_path.parent
    probes: list[tuple[float, Path]] = []
    seen: set[float] = set()
    for i, item in enumerate(directions):
        if not isinstance(item, dict) or "azimuth_deg" not in item:
            raise ValueError(f"directions[{i}] must include azimuth_deg")
        az = float(item["azimuth_deg"])
        if az in seen:
            raise ValueError(f"duplicate azimuth_deg in manifest: {az}")
        seen.add(az)
        layout = str(item.get("layout", default_layout))
        if layout == "multichannel":
            if "path" not in item:
                raise ValueError(f"directions[{i}] missing path")
            path = _resolve_path(base, str(item["path"]))
            if not path.is_file():
                raise ValueError(f"missing capture WAV: {path}")
            probes.append((az, path))
            continue
        if layout != "per_mic":
            raise ValueError(f"directions[{i}].layout must be 'multichannel' or 'per_mic'")
        channels = item.get("channels")
        if not isinstance(channels, list) or len(channels) != MICS:
            raise ValueError(f"directions[{i}].channels must be 6 paths")
        resolved = [_resolve_path(base, str(c)) for c in channels]
        missing = [p for p in resolved if not p.is_file()]
        if missing:
            listed = "\n".join(str(p) for p in missing)
            raise ValueError(f"missing per-mic WAVs:\n{listed}")
        stacked = base / f"_polar_stacked_{az:g}deg.wav"
        waves: list[np.ndarray] = []
        sr = None
        for p in resolved:
            wav, this_sr = sf.read(str(p), always_2d=True)
            if wav.shape[1] != 1:
                raise ValueError(f"{p} must be mono for per_mic layout")
            if sr is None:
                sr = int(this_sr)
            elif int(this_sr) != sr:
                raise ValueError(f"sample-rate mismatch for {p}")
            waves.append(wav[:, 0])
        n = min(w.shape[0] for w in waves)
        cube = np.stack([w[:n] for w in waves], axis=1)
        sf.write(str(stacked), cube.astype(np.float32), int(sr), subtype="FLOAT")
        probes.append((az, stacked))
    return probes


def write_synthetic_probe_wavs(
    *,
    geometry_path: Path,
    sample_rate_hz: int,
    azimuths_deg: np.ndarray,
    distance_m: float,
    duration_s: float,
    out_dir: Path,
) -> list[tuple[float, Path]]:
    """Simulate 6-channel captures with spherical delays. Not a beamformer."""
    geometry = load_geometry(geometry_path)
    n = max(int(round(duration_s * sample_rate_hz)), sample_rate_hz // 4)
    t = np.arange(n, dtype=np.float64) / float(sample_rate_hz)
    # Broadband-ish excitation so adaptive covariance is not a single bin.
    rng = np.random.default_rng(0)
    source = 0.15 * rng.standard_normal(n)
    source += 0.25 * np.sin(2.0 * np.pi * 1000.0 * t)
    out_dir.mkdir(parents=True, exist_ok=True)
    probes: list[tuple[float, Path]] = []
    for az in azimuths_deg:
        delays = geometric_delay_samples_spherical(
            geometry, sample_rate_hz, float(az), 0.0, distance_m, SPEED_OF_SOUND_MPS
        )
        u = unit_vector_from_az_el_deg(float(az), 0.0)
        src_xyz = u * float(distance_m)
        ranges = np.linalg.norm(src_xyz[None, :] - geometry.xyz_m, axis=1)
        amp = ranges[REFERENCE_INDEX] / np.maximum(ranges, 1.0e-9)
        wav = np.zeros((n, MICS), dtype=np.float32)
        for m in range(MICS):
            shift = int(np.round(delays[m]))
            if shift >= 0:
                wav[shift:, m] = (amp[m] * source[: n - shift]).astype(np.float32)
            else:
                wav[: n + shift, m] = (amp[m] * source[-shift:]).astype(np.float32)
        path = out_dir / f"synthetic_{float(az):+g}deg.wav"
        sf.write(str(path), wav, sample_rate_hz, subtype="FLOAT")
        probes.append((float(az), path))
    return probes


def beamformed_rms_dbfs(
    *,
    input_wav: Path,
    runtime_yaml: Path,
    look_az: float,
    work_dir: Path,
    binary: Path,
    settle_s: float,
) -> tuple[float, list[str]]:
    """Render one capture through host DSP; return pre-suppression beamformed RMS."""
    work_dir.mkdir(parents=True, exist_ok=True)
    script = work_dir / "steering.csv"
    script.write_text(
        "# time_s,azimuth_deg,elevation_deg,directivity_blend_deg\n"
        f"0.0,{look_az:.6f},0.0,0.0\n",
        encoding="utf-8",
    )
    processed = work_dir / "processed.wav"
    beamformed = work_dir / "beamformed.wav"
    command = [
        str(binary),
        "--input",
        str(input_wav),
        "--config",
        str(runtime_yaml),
        "--script",
        str(script),
        "--output",
        str(processed),
        "--output-beamformed",
        str(beamformed),
        "--suppression",
        "off",
        "--disable-limiter",
    ]
    proc = subprocess.run(command, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(
            "sonitude_wav_replay failed "
            f"(exit {proc.returncode}): {proc.stderr.strip() or proc.stdout.strip()}"
        )
    audio, sr = sf.read(str(beamformed), always_2d=True)
    mono = np.asarray(audio[:, 0], dtype=np.float64)
    skip = int(max(settle_s, 0.0) * float(sr))
    if mono.size > skip + sr // 20:
        mono = mono[skip:]
    rms = float(np.sqrt(np.mean(np.square(mono)) + 1e-18))
    return 20.0 * np.log10(rms + 1e-12), command


def look_normalize(azimuths: np.ndarray, rms_dbfs: np.ndarray, look_az: float) -> np.ndarray:
    look_i = int(np.argmin(np.abs(((azimuths - look_az + 180) % 360) - 180)))
    ref = float(rms_dbfs[look_i])
    return rms_dbfs - ref


def run_polar(
    probes: list[tuple[float, Path]],
    *,
    runtime_yaml: Path,
    look_az: float,
    binary: Path,
    work_dir: Path,
    settle_s: float,
) -> dict[str, Any]:
    azimuths = np.asarray([p[0] for p in probes], dtype=np.float64)
    rms = np.zeros(len(probes), dtype=np.float64)
    last_cmd: list[str] = []
    for i, (az, wav) in enumerate(probes):
        rms[i], last_cmd = beamformed_rms_dbfs(
            input_wav=wav,
            runtime_yaml=runtime_yaml,
            look_az=look_az,
            work_dir=work_dir / f"az_{az:+.1f}",
            binary=binary,
            settle_s=settle_s,
        )
    pattern = look_normalize(azimuths, rms, look_az)
    summary = summarize_suppression(azimuths, pattern, look_az)
    return {
        "look_az_deg": float(look_az),
        "azimuths_deg": azimuths.tolist(),
        "rms_dbfs": rms.tolist(),
        "pattern_db": pattern.tolist(),
        "summary": summary,
        "wav_replay": str(binary),
        "example_command": last_cmd,
        "settle_s": float(settle_s),
        "suppression": "off",
        "limiter": "disabled",
        "width_deg": 0.0,
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description="MVDR polar via sonitude_wav_replay (host MvdrBeamformer), not a Python solver"
    )
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--synthetic", action="store_true", help="geometry-delay 6-ch probes, then host DSP")
    src.add_argument("--manifest", type=str, help="YAML listing 6-channel captures per azimuth")
    root = repo_root()
    p.add_argument("--runtime", type=str, default=str(root / "config" / "default.yaml"))
    p.add_argument("--geometry", type=str, default="", help="override geometry YAML (synthetic sources only)")
    p.add_argument("--look-az", type=float, default=0.0)
    p.add_argument("--distance-m", type=float, default=None, help="synthetic source distance; default steering YAML")
    p.add_argument("--az-step", type=float, default=10.0)
    p.add_argument("--duration-s", type=float, default=1.0, help="synthetic capture length")
    p.add_argument("--settle-s", type=float, default=None, help="skip start of beamformed tap (default from ramp+cov)")
    p.add_argument("--binary", type=str, default="")
    p.add_argument("--build-dir", type=str, default="")
    p.add_argument("--work-dir", type=str, default="")
    p.add_argument("--plot", type=str, default="")
    p.add_argument("--json-out", type=str, default="")
    args = p.parse_args(argv)

    runtime_path = Path(args.runtime)
    if not runtime_path.is_file():
        runtime_path = root / args.runtime
    runtime = load_runtime_yaml(runtime_path)
    capture = runtime.get("capture") if isinstance(runtime.get("capture"), dict) else {}
    steering = runtime.get("steering") if isinstance(runtime.get("steering"), dict) else {}
    spatial = runtime.get("spatial") if isinstance(runtime.get("spatial"), dict) else {}
    mvdr = spatial.get("mvdr") if isinstance(spatial.get("mvdr"), dict) else {}
    sample_rate_hz = int(capture.get("sample_rate_hz", 44100))
    yaml_distance = float(steering.get("source_distance_m", 1.0))
    distance_m = float(yaml_distance if args.distance_m is None else args.distance_m)
    geometry_rel = str(runtime.get("geometry_path", "geometry_soundbubble_initial.yaml"))
    geometry_path = Path(args.geometry) if args.geometry else resolve_runtime_path(runtime_path, geometry_rel)
    if not geometry_path.is_file():
        geometry_path = root / "config" / geometry_path.name

    ramp_ms = float(steering.get("steering_ramp_ms", 150.0))
    cov_tau = float(mvdr.get("cov_tau_sec", 0.08))
    settle_s = float(args.settle_s) if args.settle_s is not None else (ramp_ms / 1000.0) + 3.0 * cov_tau

    binary = find_wav_replay(args.binary or None, args.build_dir or None)

    tmp_ctx = None
    if args.work_dir:
        work_dir = Path(args.work_dir)
        work_dir.mkdir(parents=True, exist_ok=True)
    else:
        tmp_ctx = tempfile.TemporaryDirectory(prefix="mvdr_polar_")
        work_dir = Path(tmp_ctx.name)

    try:
        if args.synthetic:
            azimuths = np.arange(-180.0, 180.0 + 1e-9, args.az_step)
            probes = write_synthetic_probe_wavs(
                geometry_path=geometry_path,
                sample_rate_hz=sample_rate_hz,
                azimuths_deg=azimuths,
                distance_m=distance_m,
                duration_s=float(args.duration_s),
                out_dir=work_dir / "probes",
            )
            mode = "synthetic_captures_host_dsp"
        else:
            probes = load_probe_recordings(Path(args.manifest).resolve())
            mode = "measured_captures_host_dsp"

        result = run_polar(
            probes,
            runtime_yaml=runtime_path,
            look_az=float(args.look_az),
            binary=binary,
            work_dir=work_dir / "replay",
            settle_s=settle_s,
        )
    finally:
        if tmp_ctx is not None:
            tmp_ctx.cleanup()

    report = {
        "mode": mode,
        "processor": "sonitude_wav_replay",
        "beamformer": "dsp::MvdrBeamformer"
        if str(spatial.get("backend", "adaptive_geometric")) != "fixed_measured"
        else "dsp::FixedBinauralMvdr",
        "runtime": str(runtime_path),
        "geometry": str(geometry_path),
        "spatial_backend": spatial.get("backend"),
        "steering_model": steering.get("model"),
        "source_distance_m_yaml": yaml_distance,
        "synthetic_source_distance_m": distance_m if args.synthetic else None,
        "spatial.mvdr": {
            "diag_load": mvdr.get("diag_load"),
            "max_white_noise_gain": mvdr.get("max_white_noise_gain"),
            "cov_tau_sec": mvdr.get("cov_tau_sec"),
        },
        "calibration_path": runtime.get("calibration_path"),
        "primary": result,
        "notes": [
            "No Python MVDR / Capon / delay-and-sum solver is used.",
            "Each azimuth WAV is replayed through the host DSP with a fixed look command.",
            "Pattern is look-normalized RMS of the pre-suppression beamformed tap.",
            "Tuning, geometry, steering distance, and calibration come from --runtime YAML.",
        ],
    }
    print(json.dumps(report, indent=2))
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.plot:
        title = (
            f"Host DSP MVDR look={args.look_az:.0f}°  "
            f"supp={result['summary']['suppression_db']:.1f} dB"
        )
        plot_polar(
            np.asarray(result["azimuths_deg"], dtype=np.float64),
            np.asarray(result["pattern_db"], dtype=np.float64),
            Path(args.plot),
            title,
        )
        print(f"wrote {args.plot}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
