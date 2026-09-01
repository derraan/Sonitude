#!/usr/bin/env python3
"""Synthetic noise + tone pipeline evaluation via sonitude_wav_replay."""

from __future__ import annotations

import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import soundfile as sf
import yaml

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from app.config_reader import DEFAULT_CONFIG_PATH
from app.processing.batch_adapter import run_wav_replay
from app.processing.sonitude_binary_locator import find_binary
from app.storage.models import SteeringEvent

BUILD_DIR = REPO / "build"
STFT_DELAY = 127  # 128/32 first-arrival (locked in unit tests)


@dataclass
class ToneMetrics:
    target_att_db: float
    off_att_db: float
    target_snr_db: float
    noise_rms_ratio: float
    output_rms: float


def load_geometry(path: Path) -> list[tuple[str, float, float, float]]:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    mics = []
    for mic in data["microphones"]:
        mics.append((mic["id"], float(mic["x"]), float(mic["y"]), float(mic["z"])))
    return mics


def unit_vector(az_deg: float, el_deg: float) -> np.ndarray:
    az = math.radians(az_deg)
    el = math.radians(el_deg)
    return np.array([math.cos(el) * math.cos(az), math.cos(el) * math.sin(az), math.sin(el)], dtype=np.float64)


def delay_samples(mic: np.ndarray, ref: np.ndarray, u: np.ndarray, c: float, fs: int) -> float:
    dot = float(np.dot(u, mic))
    ref_dot = float(np.dot(u, ref))
    tau_sec = -((dot - ref_dot) / c)
    return tau_sec * fs


def delay_read_linear(signal: np.ndarray, index: int, delay: float) -> float:
    pos = float(index) - delay
    i0 = int(math.floor(pos))
    i1 = i0 + 1
    frac = pos - i0
    s0 = signal[i0] if 0 <= i0 < len(signal) else 0.0
    s1 = signal[i1] if 0 <= i1 < len(signal) else 0.0
    return float((1.0 - frac) * s0 + frac * s1)


def plane_wave_frames(
    mono: np.ndarray,
    mics: list[tuple[str, float, float, float]],
    fs: int,
    az_deg: float,
    el_deg: float,
    c: float = 343.0,
    ref_index: int = 0,
) -> np.ndarray:
    u = unit_vector(az_deg, el_deg)
    positions = [np.array([x, y, z], dtype=np.float64) for _, x, y, z in mics]
    ref = positions[ref_index]
    delays = [delay_samples(p, ref, u, c, fs) for p in positions]
    n = len(mono)
    out = np.zeros((n, len(mics)), dtype=np.float32)
    for ch, d in enumerate(delays):
        for i in range(n):
            out[i, ch] = delay_read_linear(mono, i, -d)
    return out


def lcg_noise(n: int, seed: int, amp: float) -> np.ndarray:
    out = np.empty(n, dtype=np.float64)
    s = seed & 0xFFFFFFFF
    for i in range(n):
        s = (1664525 * s + 1013904223) & 0xFFFFFFFF
        u = (s >> 8) / float(1 << 24)
        out[i] = amp * ((2.0 * u) - 1.0)
    return out


def sine(n: int, fs: int, freq: float, amp: float) -> np.ndarray:
    t = np.arange(n, dtype=np.float64) / fs
    return amp * np.sin(2.0 * math.pi * freq * t)


def project_tone(
    x: np.ndarray,
    freq: float,
    fs: int,
    begin: int,
    end: int,
    time_origin: float = 0.0,
) -> tuple[float, float]:
    if end <= begin:
        return 0.0, 0.0
    w = 2.0 * math.pi * freq / fs
    target_e = 0.0
    resid_e = 0.0
    for i in range(begin, end):
        t = float(i) - time_origin
        ref = math.sin(w * t)
        ref_c = math.cos(w * t)
        sample = float(x[i])
        target_e += sample * ref
        resid_e += sample * ref_c
    target_rms = math.sqrt(target_e * target_e / (end - begin))
    resid_rms = math.sqrt(resid_e * resid_e / (end - begin))
    return target_rms, resid_rms


def coherent_snr_db(target_rms: float, resid_rms: float) -> float:
    return 20.0 * math.log10((target_rms + 1e-20) / (resid_rms + 1e-20))


def analyze_two_talker(
    beamformed: np.ndarray,
    suppressed: np.ndarray,
    fs: int,
    f_target: float,
    f_off: float,
    skip: int = STFT_DELAY + 2000,
) -> ToneMetrics:
    n = len(suppressed)
    a, b = skip, n - 500
    in_t = project_tone(beamformed, f_target, fs, a, b)
    out_t = project_tone(suppressed, f_target, fs, a, b)
    in_o = project_tone(beamformed, f_off, fs, a, b)
    out_o = project_tone(suppressed, f_off, fs, a, b)
    att_t = 20.0 * math.log10((out_t[0] + 1e-20) / (in_t[0] + 1e-20))
    att_o = 20.0 * math.log10((out_o[0] + 1e-20) / (in_o[0] + 1e-20))
    snr_in = coherent_snr_db(in_t[0], in_t[1])
    snr_out = coherent_snr_db(out_t[0], out_t[1])
    noise_ratio = float(np.sqrt(np.mean(suppressed[a:b] ** 2)) / max(np.sqrt(np.mean(beamformed[a:b] ** 2)), 1e-20))
    return ToneMetrics(att_t, att_o, snr_out - snr_in, noise_ratio, float(np.sqrt(np.mean(suppressed[a:b] ** 2))))


def write_scene(path: Path, data: np.ndarray, fs: int) -> None:
    sf.write(str(path), data.astype(np.float32), fs, subtype="FLOAT")


def run_case(
    name: str,
    wav_path: Path,
    out_dir: Path,
    suppression: str,
    backend: str | None,
    binary: Path,
) -> dict:
    events = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0)]
    kwargs: dict = {
        "suppression": suppression,
        "binary_path": binary,
        "disable_limiter": True,
    }
    if backend:
        kwargs["suppression_backend"] = backend
    result = run_wav_replay(wav_path, DEFAULT_CONFIG_PATH, events, out_dir, **kwargs)
    beam, _ = sf.read(str(result.beamformed_wav), dtype="float32")
    supp, _ = sf.read(str(result.suppressed_wav), dtype="float32")
    mono, _ = sf.read(str(result.output_wav), dtype="float32")
    return {
        "case": name,
        "suppression": suppression,
        "backend": backend or result.resolved.get("suppression_backend_resolved"),
        "resolved": result.resolved,
        "beamformed_rms": float(np.sqrt(np.mean(beam[STFT_DELAY + 2000 :] ** 2))),
        "suppressed_rms": float(np.sqrt(np.mean(supp[STFT_DELAY + 2000 :] ** 2))),
        "output_rms": float(np.sqrt(np.mean(mono[STFT_DELAY + 2000 :] ** 2))),
        "beamformed": beam,
        "suppressed": supp,
    }


def main() -> int:
    config_path = Path(DEFAULT_CONFIG_PATH)
    cfg = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    fs = int(cfg["capture"]["sample_rate_hz"])
    geom_path = (config_path.parent / cfg["geometry_path"]).resolve()
    mics = load_geometry(geom_path)
    ref_index = int(cfg["steering"]["reference_mic_index"])
    c = float(cfg["steering"]["speed_of_sound_mps"])

    binary = find_binary("sonitude_wav_replay", BUILD_DIR)
    out_root = REPO / "demo_mvdr" / "eval_synth"
    out_root.mkdir(parents=True, exist_ok=True)

    duration_s = 1.0
    n = int(fs * duration_s)
    f_target = 4.0 * fs / 128.0
    f_off = 8.0 * fs / 128.0

    # Scene 1: broadband noise only (equal on all channels)
    noise = lcg_noise(n, 42, 0.2)
    noise_scene = np.stack([noise.astype(np.float32)] * 6, axis=1)

    # Scene 2: on-axis tone + noise mixture (noise lead-in)
    lead = 8000
    tone = sine(n, fs, f_target, 0.25)
    noise2 = lcg_noise(n, 99, 0.15)
    mix_mono = noise2.copy()
    mix_mono[lead:] += tone[lead:]
    tone_noise_scene = plane_wave_frames(mix_mono, mics, fs, 0.0, 0.0, c, ref_index)

    # Scene 3: two-talker (0° target + 90° interferer), STFT bin-aligned
    src_t = sine(n, fs, f_target, 1.0)
    src_o = sine(n, fs, f_off, 1.0)
    mic_t = plane_wave_frames(src_t, mics, fs, 0.0, 0.0, c, ref_index)
    mic_o = plane_wave_frames(src_o, mics, fs, 90.0, 0.0, c, ref_index)
    two_talker = 0.35 * mic_t + 0.35 * mic_o

    scenes = {
        "noise_only": noise_scene,
        "tone_noise_leadin": tone_noise_scene,
        "two_talker": two_talker,
    }

    modes = [
        ("beamform_only", "off", None),
        ("conservative", "on", "conservative"),
        ("spectral", "on", "spectral"),
    ]

    report: dict = {"sample_rate_hz": fs, "stft_delay_samples": STFT_DELAY, "scenes": {}}

    for scene_name, data in scenes.items():
        wav_path = out_root / f"{scene_name}.wav"
        write_scene(wav_path, data, fs)
        scene_report: dict = {"modes": {}}
        beam_only = None
        for mode_name, sup, backend in modes:
            mode_dir = out_root / scene_name / mode_name
            mode_dir.mkdir(parents=True, exist_ok=True)
            row = run_case(scene_name, wav_path, mode_dir, sup, backend, binary)
            rms_change_db = 20.0 * math.log10(
                (row["suppressed_rms"] + 1e-20) / (row["beamformed_rms"] + 1e-20)
            )
            entry = {
                "beamformed_rms": row["beamformed_rms"],
                "suppressed_rms": row["suppressed_rms"],
                "rms_change_db": rms_change_db,
                "backend_resolved": row["backend"],
            }
            if scene_name == "two_talker":
                metrics = analyze_two_talker(row["beamformed"], row["suppressed"], fs, f_target, f_off)
                entry.update(
                    {
                        "target_att_db": metrics.target_att_db,
                        "off_axis_att_db": metrics.off_att_db,
                        "off_minus_target_att_db": metrics.off_att_db - metrics.target_att_db,
                        "coherent_snr_gain_db": metrics.target_snr_db,
                    }
                )
            if mode_name == "beamform_only":
                beam_only = row
            scene_report["modes"][mode_name] = entry
        scene_report["modes"]["conservative"]["vs_beamform_rms_db"] = 20.0 * math.log10(
            (scene_report["modes"]["conservative"]["suppressed_rms"] + 1e-20)
            / (scene_report["modes"]["beamform_only"]["suppressed_rms"] + 1e-20)
        )
        scene_report["modes"]["spectral"]["vs_beamform_rms_db"] = 20.0 * math.log10(
            (scene_report["modes"]["spectral"]["suppressed_rms"] + 1e-20)
            / (scene_report["modes"]["beamform_only"]["suppressed_rms"] + 1e-20)
        )
        if scene_name == "two_talker" and beam_only is not None:
            spec = scene_report["modes"]["spectral"]
            cons = scene_report["modes"]["conservative"]
            scene_report["spectral_vs_conservative"] = {
                "extra_off_axis_suppression_db": cons["off_axis_att_db"] - spec["off_axis_att_db"],
                "target_preservation_delta_db": spec["target_att_db"] - cons["target_att_db"],
            }
        report["scenes"][scene_name] = scene_report

    report_path = out_root / "report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    print(f"\nWrote {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
