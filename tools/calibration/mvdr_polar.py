"""Near-field MVDR beampattern / polar diagnostics from impulse responses.

Evaluates fixed-look MVDR weights against measured or synthetic ATFs and
reports (and optionally plots) the polar response. Delay-and-sum is never used:
weights come from loaded_mvdr only.

Typical investigation:
  python -m tools.calibration.mvdr_polar --synthetic --look-az 0 --plot out.png
  python -m tools.calibration.mvdr_polar --manifest path.yaml --look-az 0
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

import numpy as np

try:
    from .compile_array import (
        compile_from_irs,
        dtft,
        load_ir_cube,
        load_manifest,
        loaded_mvdr,
        make_synthetic_impulse_cube,
        regularized_rtf,
    )
    from .geometry import (
        REFERENCE_INDEX,
        SPEED_OF_SOUND_MPS,
        geometric_delay_samples_spherical,
        load_geometry,
        unit_vector_from_az_el_deg,
    )
except ImportError:  # pragma: no cover
    from compile_array import (  # type: ignore
        compile_from_irs,
        dtft,
        load_ir_cube,
        load_manifest,
        loaded_mvdr,
        make_synthetic_impulse_cube,
        regularized_rtf,
    )
    from geometry import (  # type: ignore
        REFERENCE_INDEX,
        SPEED_OF_SOUND_MPS,
        geometric_delay_samples_spherical,
        load_geometry,
        unit_vector_from_az_el_deg,
    )

MICS = 6


def near_field_atf(
    geometry_path: str | Path,
    sample_rate_hz: int,
    azimuths_deg: np.ndarray,
    *,
    distance_m: float,
    fft_size: int,
    reference_mic: int = REFERENCE_INDEX,
    include_amplitude: bool = True,
) -> np.ndarray:
    """Complex near-field ATF cube [dir, bin, mic] from spherical delays (+ 1/r)."""
    geometry = load_geometry(geometry_path)
    n_bins = (fft_size // 2) + 1
    out = np.zeros((len(azimuths_deg), n_bins, MICS), dtype=np.complex128)
    k = np.arange(n_bins, dtype=np.float64)
    for di, az in enumerate(azimuths_deg):
        delays = geometric_delay_samples_spherical(
            geometry,
            sample_rate_hz,
            float(az),
            0.0,
            distance_m,
            SPEED_OF_SOUND_MPS,
        )
        u = unit_vector_from_az_el_deg(float(az), 0.0)
        source = u * float(distance_m)
        ranges = np.linalg.norm(source[None, :] - geometry.xyz_m, axis=1)
        amp = (ranges[reference_mic] / np.maximum(ranges, 1.0e-9)) if include_amplitude else np.ones(MICS)
        for m in range(MICS):
            phase = np.exp(-2.0j * np.pi * k * delays[m] / float(fft_size))
            out[di, :, m] = amp[m] * phase
    return out


def atf_from_irs(irs: np.ndarray, fft_size: int) -> np.ndarray:
    """DTFT of IR cube [dir, mic, time] -> [dir, bin, mic]."""
    n_dir = irs.shape[0]
    n_bins = (fft_size // 2) + 1
    h = np.zeros((n_dir, n_bins, MICS), dtype=np.complex128)
    for di in range(n_dir):
        for m in range(MICS):
            h[di, :, m] = dtft(irs[di, m], fft_size, n_bins)
    return h


def noise_covariance(
    h_all: np.ndarray,
    *,
    look_index: int | None,
    exclude_look: bool,
    self_noise: float,
) -> np.ndarray:
    """Angular ATF outer-product noise model [bin, mic, mic]."""
    n_dir, n_bins, _ = h_all.shape
    gamma = np.zeros((n_bins, MICS, MICS), dtype=np.complex128)
    indices = [i for i in range(n_dir) if not (exclude_look and look_index is not None and i == look_index)]
    if not indices:
        indices = list(range(n_dir))
    p = 1.0 / float(len(indices))
    for k in range(n_bins):
        acc = np.zeros((MICS, MICS), dtype=np.complex128)
        for di in indices:
            h = h_all[di, k, :][:, None]
            acc += p * (h @ h.conj().T)
        acc += self_noise * np.eye(MICS)
        gamma[k] = 0.5 * (acc + acc.conj().T)
    return gamma


def mvdr_weights_for_look(
    h_all: np.ndarray,
    look_index: int,
    *,
    reference_mic: int,
    exclude_look: bool,
    self_noise: float,
    max_weight_norm: float,
    diag_load: float,
) -> tuple[np.ndarray, np.ndarray]:
    """Pure loaded MVDR weights for one look (no DAS). Returns w[bin,mic], d[bin,mic]."""
    n_bins = h_all.shape[1]
    d_m, valid = regularized_rtf(np.moveaxis(h_all[look_index], 0, 1), reference_mic, 1e-8)
    d = np.moveaxis(d_m, 0, 1)
    gamma = noise_covariance(h_all, look_index=look_index, exclude_look=exclude_look, self_noise=self_noise)
    w = np.zeros_like(d)
    for k in range(n_bins):
        if not valid[k]:
            w[k] = d[k] / (np.vdot(d[k], d[k]) + 1e-18)
            continue
        lam = max(float(diag_load), 1e-4)
        wk = None
        for _ in range(16):
            wk = loaded_mvdr(gamma[k], d[k], lam)
            if np.vdot(wk, wk).real <= max_weight_norm:
                break
            lam *= 3.0
        assert wk is not None
        unity = np.vdot(d[k], wk)
        if abs(unity - 1.0) > 1e-3 and abs(unity) > 1e-12:
            wk = wk / np.conj(unity)
        w[k] = wk
    return w, d


def beampattern_db(
    w: np.ndarray,
    h_probe: np.ndarray,
    *,
    look_index: int,
    speech_bins: slice | None = None,
) -> np.ndarray:
    """Per-direction response 20 log10 |w^H h|, normalized to the look direction."""
    n_dir = h_probe.shape[0]
    bins = speech_bins or slice(None)
    resp = np.zeros(n_dir, dtype=np.float64)
    for di in range(n_dir):
        vals = []
        for k in range(w.shape[0])[bins]:
            vals.append(abs(np.vdot(w[k], h_probe[di, k, :])))
        resp[di] = float(np.mean(vals)) if vals else 0.0
    ref = max(float(resp[look_index]), 1e-18)
    return 20.0 * np.log10(np.maximum(resp / ref, 1e-18))


def speech_bin_slice(fft_size: int, sample_rate_hz: int, low_hz: float = 300.0, high_hz: float = 4000.0) -> slice:
    n_bins = (fft_size // 2) + 1
    df = sample_rate_hz / float(fft_size)
    lo = max(1, int(math.floor(low_hz / df)))
    hi = min(n_bins - 1, int(math.ceil(high_hz / df)))
    return slice(lo, hi + 1)


def summarize_suppression(azimuths: np.ndarray, pattern_db: np.ndarray, look_az: float) -> dict[str, float]:
    look_i = int(np.argmin(np.abs(((azimuths - look_az + 180) % 360) - 180)))
    look_db = float(pattern_db[look_i])
    peak_i = int(np.argmax(pattern_db))
    # Off-look: angles at least 60° away.
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
    # matplotlib default polar is 0° at East. We set theta_zero='N' before plot.
    theta = np.deg2rad(az)
    floor = float(np.min(pat))
    radius = pat - floor + 1.0
    # Close the contour for fill/plot.
    theta = np.concatenate([theta, theta[:1]])
    radius = np.concatenate([radius, radius[:1]])
    return theta, radius


def plot_polar(azimuths_deg: np.ndarray, pattern_db: np.ndarray, out_path: Path, title: str) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    theta, radius = polar_plot_coords(azimuths_deg, pattern_db)
    fig, ax = plt.subplots(subplot_kw={"projection": "polar"}, figsize=(7, 7))
    # Set the compass before drawing so saved figures cannot inherit the
    # matplotlib default (0° at East, counterclockwise).
    ax.set_theta_zero_location("N")
    ax.set_theta_direction(-1)
    ax.set_thetagrids(
        [0, 45, 90, 135, 180, 225, 270, 315],
        labels=["0° front", "45°", "90° right", "135°", "180° back", "225°", "270° left", "315°"],
    )
    ax.plot(theta, radius, color="#0b3d5c", linewidth=2.0)
    ax.fill(theta, radius, color="#0b3d5c", alpha=0.18)
    ax.set_title(title)
    ax.set_ylim(0, float(np.max(radius)) * 1.05)
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=140)
    plt.close(fig)


def _synthetic_nearfield_irs(
    geometry_path: Path,
    sample_rate_hz: int,
    azimuths: np.ndarray,
    distance_m: float,
    n_taps: int = 256,
) -> np.ndarray:
    """Band-limited near-field delay impulses for polar self-test."""
    geometry = load_geometry(geometry_path)
    cube = np.zeros((len(azimuths), MICS, n_taps), dtype=np.float64)
    t = np.arange(n_taps)
    for di, az in enumerate(azimuths):
        delays = geometric_delay_samples_spherical(
            geometry, sample_rate_hz, float(az), 0.0, distance_m, SPEED_OF_SOUND_MPS
        )
        u = unit_vector_from_az_el_deg(float(az), 0.0)
        source = u * float(distance_m)
        ranges = np.linalg.norm(source[None, :] - geometry.xyz_m, axis=1)
        amp = ranges[REFERENCE_INDEX] / np.maximum(ranges, 1.0e-9)
        for m in range(MICS):
            center = 48.0 + delays[m]
            cube[di, m] = amp[m] * np.exp(-0.5 * ((t - center) / 1.8) ** 2)
    return cube


def run_case(
    *,
    h_all: np.ndarray,
    azimuths: np.ndarray,
    look_az: float,
    sample_rate_hz: int,
    fft_size: int,
    reference_mic: int,
    exclude_look: bool,
    self_noise: float,
    max_weight_norm: float,
    diag_load: float,
) -> dict[str, Any]:
    look_index = int(np.argmin(np.abs(((azimuths - look_az + 180) % 360) - 180)))
    w, _d = mvdr_weights_for_look(
        h_all,
        look_index,
        reference_mic=reference_mic,
        exclude_look=exclude_look,
        self_noise=self_noise,
        max_weight_norm=max_weight_norm,
        diag_load=diag_load,
    )
    bins = speech_bin_slice(fft_size, sample_rate_hz)
    pattern = beampattern_db(w, h_all, look_index=look_index, speech_bins=bins)
    summary = summarize_suppression(azimuths, pattern, float(azimuths[look_index]))
    return {
        "look_index": look_index,
        "azimuths_deg": azimuths.tolist(),
        "pattern_db": pattern.tolist(),
        "summary": summary,
        "exclude_look": exclude_look,
        "max_weight_norm": max_weight_norm,
        "diag_load": diag_load,
        "self_noise": self_noise,
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Near-field MVDR polar / beampattern from IRs")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--synthetic", action="store_true", help="use geometric near-field synthetic IRs")
    src.add_argument("--manifest", type=str, help="IR/sweep manifest for measured ATFs")
    p.add_argument("--geometry", type=str, default="config/geometry_soundbubble_initial.yaml")
    p.add_argument("--look-az", type=float, default=0.0)
    p.add_argument("--distance-m", type=float, default=1.0, help="near-field look/measurement distance")
    p.add_argument("--sample-rate", type=int, default=44100)
    p.add_argument("--fft-size", type=int, default=128)
    p.add_argument("--reference-mic", type=int, default=REFERENCE_INDEX)
    p.add_argument("--self-noise", type=float, default=1e-3)
    p.add_argument("--max-weight-norm", type=float, default=32.0)
    p.add_argument("--diag-load", type=float, default=1e-4)
    p.add_argument(
        "--include-look-in-noise",
        action="store_true",
        help="match compile_array.py (look ATF included in Γ). Default excludes look.",
    )
    p.add_argument("--az-step", type=float, default=10.0)
    p.add_argument("--plot", type=str, default="", help="optional PNG path")
    p.add_argument("--json-out", type=str, default="", help="optional JSON report path")
    args = p.parse_args(argv)

    if args.synthetic:
        azimuths = np.arange(-180.0, 180.0 + 1e-9, args.az_step)
        irs = _synthetic_nearfield_irs(
            Path(args.geometry), args.sample_rate, azimuths, args.distance_m
        )
        h_all = atf_from_irs(irs, args.fft_size)
        # Also emit a compile_array profile for parity checks when useful.
        _ = compile_from_irs
    else:
        manifest_path = Path(args.manifest).resolve()
        manifest = load_manifest(manifest_path)
        irs, az_list, sr, _meta = load_ir_cube(manifest, manifest_path.parent)
        args.sample_rate = int(sr)
        args.fft_size = int(manifest.get("fft_size", args.fft_size))
        args.reference_mic = int(manifest.get("reference_mic", args.reference_mic))
        azimuths = np.asarray(az_list, dtype=np.float64)
        h_all = atf_from_irs(irs, args.fft_size)

    exclude_look = not args.include_look_in_noise
    result = run_case(
        h_all=h_all,
        azimuths=azimuths,
        look_az=args.look_az,
        sample_rate_hz=args.sample_rate,
        fft_size=args.fft_size,
        reference_mic=args.reference_mic,
        exclude_look=exclude_look,
        self_noise=args.self_noise,
        max_weight_norm=args.max_weight_norm,
        diag_load=args.diag_load,
    )

    # Compare against include-look (compile_array default) when investigating.
    compare = run_case(
        h_all=h_all,
        azimuths=azimuths,
        look_az=args.look_az,
        sample_rate_hz=args.sample_rate,
        fft_size=args.fft_size,
        reference_mic=args.reference_mic,
        exclude_look=False,
        self_noise=args.self_noise,
        max_weight_norm=args.max_weight_norm,
        diag_load=args.diag_load,
    )

    report = {
        "mode": "synthetic_near_field" if args.synthetic else "measured_ir",
        "geometry": str(args.geometry),
        "distance_m": args.distance_m,
        "primary": result,
        "compile_array_style_include_look": compare["summary"],
        "notes": [
            "Weights are pure loaded MVDR (no delay-and-sum).",
            "Steering uses near-field spherical delays when --synthetic.",
            "Default noise model excludes the look ATF from Γ so Capon nulls can form.",
            "Pass --include-look-in-noise to match the older all-direction Γ average.",
            "Geometry must use head_frame axes (+X right, +Y forward); a swapped frame steers az=0 toward +90°.",
        ],
    }

    print(json.dumps(report, indent=2))
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.plot:
        title = (
            f"Near-field MVDR look={args.look_az:.0f}°  "
            f"supp={result['summary']['suppression_db']:.1f} dB"
        )
        plot_polar(azimuths, np.asarray(result["pattern_db"]), Path(args.plot), title)
        print(f"wrote {args.plot}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
