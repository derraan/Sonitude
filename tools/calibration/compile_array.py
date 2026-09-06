"""Compile synchronized array IRs into a Sonitude SMV3 coefficient artifact.

This tool does not control REW or Audacity. It starts from exported impulse
responses plus a JSON/YAML manifest.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np

MAGIC = b"SMV3"
HEADER_SIZE = 128
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MICS = 6


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
    return (np.exp(1j * phase) @ h.astype(np.complex128))


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
    header[96] = 1 if profile.get("synthetic", True) else 0
    header[97] = int(profile.get("noise_model", 0))
    struct.pack_into("<f", header, 100, float(profile.get("reference_gain", 1.0)))
    digest = fnv1a64(payload)
    struct.pack_into("<I", header, 80, digest & 0xFFFFFFFF)
    struct.pack_into("<I", header, 84, digest >> 32)
    return bytes(header) + payload


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
) -> dict:
    if irs.ndim != 3 or irs.shape[1] != MICS:
        raise ValueError("irs must be [dir, 6, time]")
    n_dir, _, n_time = irs.shape
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

    gamma = np.zeros((n_bins, MICS, MICS), dtype=np.complex128)
    p = 1.0 / float(n_dir)
    for k in range(n_bins):
        acc = np.zeros((MICS, MICS), dtype=np.complex128)
        for di in range(n_dir):
            h = h_all[di, k, :][:, None]
            acc += p * (h @ h.conj().T)
        acc += self_noise * np.eye(MICS)
        gamma[k] = 0.5 * (acc + acc.conj().T)

    w_all = np.zeros_like(d_all)
    for di in range(n_dir):
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
    }


def write_outputs(profile: dict, out_prefix: Path) -> None:
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
        "synthetic": True,
        "directions": int(profile["direction_count"]),
        "bins": int(profile["bin_count"]),
        "bytes": len(blob),
        "noise_model": "angular_atf_plus_documented_self_noise",
        "self_noise_note": "Identity loading is a documented synthetic floor, not a measured sensor noise PSD.",
    }
    (out_prefix.with_suffix(".report.json")).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def make_synthetic_impulse_cube(sample_rate_hz: int, delays: list[list[float]]) -> np.ndarray:
    n = 512
    cube = np.zeros((len(delays), MICS, n), dtype=np.float64)
    t = np.arange(n)
    for di, drow in enumerate(delays):
        for m, delay in enumerate(drow):
            cube[di, m] = np.exp(-0.5 * ((t - (64.0 + delay)) / 2.5) ** 2)
    return cube


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Sonitude array calibration compiler")
    parser.add_argument("--synthetic", action="store_true", help="emit a labeled synthetic fixture")
    parser.add_argument("--output-prefix", required=True)
    parser.add_argument("--sample-rate", type=int, default=44100)
    args = parser.parse_args(argv)
    if not args.synthetic:
        print("Physical IR import requires a manifest; use --synthetic for CI fixtures.", file=sys.stderr)
        return 2
    delays = [
        [0.0, 0.4, 0.8, 1.2, 1.6, 2.0],
        [0.5, 0.0, 0.4, 0.9, 1.3, 1.8],
    ]
    irs = make_synthetic_impulse_cube(args.sample_rate, delays)
    profile = compile_from_irs(irs, [0.0, 10.0], args.sample_rate)
    write_outputs(profile, Path(args.output_prefix))
    print(f"wrote synthetic profile prefix {args.output_prefix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
