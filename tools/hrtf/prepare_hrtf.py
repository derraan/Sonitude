#!/usr/bin/env python3
"""
Prepare Sonitude HRTF tables from a SOFA file.

This tool emits Sonitude SNHR v1 tables for a 256-tap reference and for
experimental compact-HRIR *candidates* (first N raw samples). Compact tables
are not a validated edge representation until reference-error measurements exist.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import struct
import zlib

import numpy as np


def resolve_target_rate(source_rate: int, target_rate: int) -> int:
    """Reject relabel-without-resample. HRIR resampling is not implemented."""
    if int(target_rate) != int(source_rate):
        raise RuntimeError(
            f"--target-rate {target_rate} does not match SOFA sample rate {source_rate}. "
            "HRIR resampling is not implemented; omit --target-rate or pass the source rate."
        )
    return int(source_rate)


def normalize_azimuth_deg(az: float) -> float:
    out = math.fmod(az, 360.0)
    if out > 180.0:
        out -= 360.0
    if out <= -180.0:
        out += 360.0
    return out


def build_table(
    sofa_path: pathlib.Path, out_path: pathlib.Path, taps: int, target_rate: int, sonitude_step_deg: int
) -> dict:
    import h5py

    with h5py.File(sofa_path, "r") as f:
        sr = int(np.asarray(f["Data.SamplingRate"])[0])
        ir = np.asarray(f["Data.IR"])  # [M, R, N]
        pos = np.asarray(f["SourcePosition"])  # [M,3] deg,deg,m

    target_rate = resolve_target_rate(sr, target_rate)

    if ir.shape[1] != 2:
        raise RuntimeError("SOFA must contain exactly 2 receiver channels")

    # SADIE azimuth is anti-clockwise positive with 0=front.
    # Sonitude uses clockwise-positive with 0=front. Convert by negating azimuth.
    pos_az = np.vectorize(normalize_azimuth_deg)(-pos[:, 0].astype(np.float64))
    pos_el = pos[:, 1].astype(np.float64)
    horizontal = np.where(np.abs(pos_el) < 1.0)[0]
    if horizontal.size == 0:
        raise RuntimeError("No near-horizontal measurements found in SOFA")

    directions = []
    fir = []
    for az in range(-180, 180, sonitude_step_deg):
        wrapped = normalize_azimuth_deg(float(az))
        idx = horizontal[np.argmin(np.abs(pos_az[horizontal] - wrapped))]
        h = ir[idx, :, :].astype(np.float32)
        h = h[:, :taps]
        directions.append([wrapped, 0.0, 0.0, 0.0])
        fir.append(h[0, :])
        fir.append(h[1, :])

    coeff = np.stack(fir, axis=0).reshape(len(directions), 2, taps)
    max_abs = np.max(np.abs(coeff))
    if max_abs > 1.0:
        coeff = coeff / max_abs

    payload = bytearray()
    payload += struct.pack("<I", 1)  # version
    payload += struct.pack("<I", target_rate)
    payload += struct.pack("<I", len(directions))
    payload += struct.pack("<I", taps)
    payload += struct.pack("<I", 2)
    for d in directions:
        payload += struct.pack("<ffff", *d)
    payload += coeff.astype(np.float32).tobytes(order="C")
    crc = zlib.crc32(payload) & 0xFFFFFFFF

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("wb") as f:
        f.write(b"SNHR")
        f.write(payload)
        f.write(struct.pack("<I", crc))

    return {
        "source_sample_rate_hz": sr,
        "target_sample_rate_hz": target_rate,
        "direction_count": len(directions),
        "taps_per_ear": taps,
        "reduction_method": "raw_hrir_prefix_truncation" if taps < 256 else "full_hrir_prefix",
        "status": (
            "experimental_compact_hrir_candidate"
            if taps < 256
            else "full_hrtf_reference"
        ),
        "edge_ready": False,
        "crc32": f"0x{crc:08x}",
        "path": str(out_path.as_posix()),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-sofa", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--target-rate", type=int, default=44100)
    parser.add_argument("--azimuth-step", type=int, default=5)
    args = parser.parse_args()

    sofa_path = pathlib.Path(args.input_sofa)
    out_dir = pathlib.Path(args.out_dir)

    outputs = {}
    for taps, name in [(16, "compact_16.shrf"), (32, "compact_32.shrf"), (64, "compact_64.shrf"), (256, "reference.shrf")]:
        outputs[name] = build_table(
            sofa_path=sofa_path,
            out_path=out_dir / name,
            taps=taps,
            target_rate=args.target_rate,
            sonitude_step_deg=args.azimuth_step,
        )

    provenance = {
        "dataset": "SADIE II",
        "subject": "D2",
        "profile_id": "generic_sadie2_d2",
        "license": "Apache-2.0",
        "source_format": "SOFA/AES69",
        "preparation_tool": "tools/hrtf/prepare_hrtf.py",
        "preparation_version": "2",
        "azimuth_mapping": "sonitude_az = normalize(-sadie_az)",
        "compact_note": (
            "compact_16/32/64 are the first N raw HRIR samples with explicit ITD left at "
            "zero. They are experimental candidates, not a selected edge representation."
        ),
        "outputs": outputs,
    }
    (out_dir / "provenance.json").write_text(json.dumps(provenance, indent=2), encoding="utf-8")
    print(f"Wrote HRTF tables into {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
