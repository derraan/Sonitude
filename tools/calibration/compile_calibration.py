from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from scipy import signal

from .emit_yaml import emit_variants
from .estimate_delay import DelayBandEstimate, estimate_delays_by_band
from .estimate_gain import estimate_gain_linear
from .estimate_polarity import polarity_from_delay_peak
from .estimate_tof import (
    default_max_tof_samples,
    estimate_absolute_tof_by_band,
    relative_lags_from_tof,
)
from .geometry import (
    MIC_IDS,
    REFERENCE_INDEX,
    SPEED_OF_SOUND_MPS,
    geometric_delay_samples_plane,
    geometric_delay_samples_spherical,
    load_geometry,
)
from .io_wav import read_wav, split_channels
from .report import write_report
from .rew_filters import RewFilter, parse_rew_filter_txt


@dataclass(frozen=True)
class CalibrationCompileRequest:
    array_wav: Path
    element_wavs: tuple[Path, Path, Path, Path, Path, Path]
    geometry: Path
    out_dir: Path
    tag: str = "session"
    azimuth_deg: float = 0.0
    elevation_deg: float = 0.0
    distance_m: float = 1.0
    wavefront: str = "spherical"
    max_lag_samples: int = 100
    polarity_threshold: float = 0.2
    gain_source: str = "element"
    rew_filter_txt: str = ""
    rew_max_q: float = 4.0
    rew_max_boost_db: float = 6.0
    rew_min_freq_hz: float = 100.0
    ch6_invert_test: bool = True
    # Additional enabled array takes (azimuth_deg, wav_path). Primary remains array_wav/azimuth_deg.
    extra_array_angles: tuple[tuple[float, Path], ...] = ()
    stimulus_wav: str = ""
    # absolute_tof requires stimulus_wav; relative is mic-vs-mic GCC-PHAT.
    delay_mode: str = "absolute_tof"
    max_tof_samples: int = 0  # 0 => auto from distance + 100 ms margin


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Compile Sonitude calibration artifacts from measurement WAVs.")
    p.add_argument("--array-wav", required=True)
    p.add_argument("--element-wavs", nargs=6, required=True)
    p.add_argument("--geometry", required=True)
    p.add_argument("--out-dir", required=True)
    p.add_argument("--tag", default="session")
    p.add_argument("--azimuth-deg", type=float, default=0.0)
    p.add_argument("--elevation-deg", type=float, default=0.0)
    p.add_argument("--distance-m", type=float, default=1.0)
    p.add_argument("--wavefront", choices=("spherical", "plane"), default="spherical")
    p.add_argument("--max-lag-samples", type=int, default=100)
    p.add_argument("--polarity-threshold", type=float, default=0.2)
    p.add_argument("--gain-source", choices=("element", "array"), default="element")
    p.add_argument("--rew-filter-txt", default="")
    p.add_argument("--rew-max-q", type=float, default=4.0)
    p.add_argument("--rew-max-boost-db", type=float, default=6.0)
    p.add_argument("--rew-min-freq-hz", type=float, default=100.0)
    p.add_argument("--no-m5-invert-test", action="store_true")
    p.add_argument(
        "--extra-array-angle",
        nargs=2,
        action="append",
        default=[],
        metavar=("AZIMUTH_DEG", "WAV"),
        help="Optional additional array WAV at another azimuth (repeatable).",
    )
    p.add_argument("--stimulus-wav", default="", help="Played stimulus WAV for absolute TOF.")
    p.add_argument(
        "--delay-mode",
        choices=("absolute_tof", "relative"),
        default="absolute_tof",
        help="absolute_tof needs --stimulus-wav; relative is mic-vs-mic GCC-PHAT.",
    )
    p.add_argument(
        "--max-tof-samples",
        type=int,
        default=0,
        help="Absolute TOF search window (0 = auto from distance + 100 ms).",
    )
    return p.parse_args()


def request_from_args(args: argparse.Namespace) -> CalibrationCompileRequest:
    element = tuple(Path(p) for p in args.element_wavs)
    if len(element) != 6:
        raise RuntimeError("Expected six element WAV paths")
    extras: list[tuple[float, Path]] = []
    for pair in args.extra_array_angle or []:
        extras.append((float(pair[0]), Path(pair[1])))
    return CalibrationCompileRequest(
        array_wav=Path(args.array_wav),
        element_wavs=element,  # type: ignore[arg-type]
        geometry=Path(args.geometry),
        out_dir=Path(args.out_dir),
        tag=str(args.tag),
        azimuth_deg=float(args.azimuth_deg),
        elevation_deg=float(args.elevation_deg),
        distance_m=float(args.distance_m),
        wavefront=str(args.wavefront),
        max_lag_samples=int(args.max_lag_samples),
        polarity_threshold=float(args.polarity_threshold),
        gain_source=str(args.gain_source),
        rew_filter_txt=str(args.rew_filter_txt),
        rew_max_q=float(args.rew_max_q),
        rew_max_boost_db=float(args.rew_max_boost_db),
        rew_min_freq_hz=float(args.rew_min_freq_hz),
        ch6_invert_test=(not args.no_m5_invert_test),
        extra_array_angles=tuple(extras),
        stimulus_wav=str(args.stimulus_wav),
        delay_mode=str(args.delay_mode),
        max_tof_samples=int(args.max_tof_samples),
    )


def _validate_runtime_bounds(delays: list[float], gains: list[float], polarity: list[int]) -> None:
    if len(delays) != 6 or len(gains) != 6 or len(polarity) != 6:
        raise RuntimeError("Calibration vectors must all be length 6")
    for p in polarity:
        if p not in (-1, 1):
            raise RuntimeError("polarity must be +1 or -1")
    for g in gains:
        if not (0.0 < g <= 8.0):
            raise RuntimeError(f"gain out of range (0,8]: {g}")
    for d in delays:
        if abs(d) > 256.0:
            raise RuntimeError(f"delay_samples out of range +/-256: {d}")


def _band_rms(signal_1d: np.ndarray, fs: int, f0: float, f1: float) -> float:
    nyq = 0.5 * float(fs)
    b, a = signal.butter(4, [max(f0, 1.0) / nyq, min(f1, nyq * 0.99) / nyq], btype="bandpass")
    x = signal.filtfilt(b, a, signal_1d)
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float64), dtype=np.float64)))


def _estimate_per_mic_eq(element_channels: list[np.ndarray], sample_rate_hz: int) -> list[list[RewFilter]]:
    # Fit low-order matching EQ from broad-band differences only, capped for robustness.
    ref = element_channels[REFERENCE_INDEX]
    bands = [(650.0, (300.0, 1000.0)), (2000.0, (1000.0, 3000.0)), (5200.0, (3000.0, 8000.0))]
    out: list[list[RewFilter]] = []
    for ch in element_channels:
        sections: list[RewFilter] = []
        for center_hz, (f0, f1) in bands:
            rr = max(_band_rms(ref, sample_rate_hz, f0, f1), 1.0e-12)
            cc = max(_band_rms(ch, sample_rate_hz, f0, f1), 1.0e-12)
            diff_db = float(20.0 * np.log10(cc / rr))
            gain_db = float(np.clip(-diff_db, -6.0, 6.0))
            if abs(gain_db) < 0.5:
                continue
            sections.append(RewFilter(ftype="PK", freq_hz=center_hz, gain_db=gain_db, q=1.2))
        out.append(sections)
    return out


def _geometry_tau(
    geometry,
    sample_rate_hz: int,
    *,
    azimuth_deg: float,
    elevation_deg: float,
    distance_m: float,
    wavefront: str,
) -> np.ndarray:
    if wavefront == "spherical":
        return geometric_delay_samples_spherical(
            geometry=geometry,
            sample_rate_hz=sample_rate_hz,
            azimuth_deg=azimuth_deg,
            elevation_deg=elevation_deg,
            distance_m=distance_m,
            speed_of_sound_mps=SPEED_OF_SOUND_MPS,
        )
    return geometric_delay_samples_plane(
        geometry=geometry,
        sample_rate_hz=sample_rate_hz,
        azimuth_deg=azimuth_deg,
        elevation_deg=elevation_deg,
        speed_of_sound_mps=SPEED_OF_SOUND_MPS,
    )


def _estimate_array_angle(
    *,
    array_channels: list[np.ndarray],
    sample_rate_hz: int,
    geometry,
    azimuth_deg: float,
    elevation_deg: float,
    distance_m: float,
    wavefront: str,
    max_lag_samples: int,
    wav_path: Path,
    role: str,
    delay_mode: str,
    stimulus: np.ndarray | None,
    max_tof_samples: int,
) -> dict[str, Any]:
    tau_geom = _geometry_tau(
        geometry,
        sample_rate_hz,
        azimuth_deg=azimuth_deg,
        elevation_deg=elevation_deg,
        distance_m=distance_m,
        wavefront=wavefront,
    )
    chosen_band = "300_8000"
    absolute_tof_rows: list[dict[str, Any]] | None = None

    if delay_mode == "absolute_tof":
        if stimulus is None:
            raise RuntimeError("absolute_tof delay mode requires a stimulus WAV")
        tof_by_band = estimate_absolute_tof_by_band(
            stimulus,
            array_channels,
            sample_rate_hz,
            max_tof_samples=max_tof_samples,
            min_tof_samples=0,
        )
        tofs = tof_by_band[chosen_band]
        chosen = relative_lags_from_tof(tofs, REFERENCE_INDEX)
        absolute_tof_rows = [
            {
                "id": MIC_IDS[idx],
                "tof_samples": float(tofs[idx].tof_samples),
                "tof_ms": float(tofs[idx].tof_samples) * 1000.0 / float(sample_rate_hz),
                "corr_peak": float(tofs[idx].peak_signed_corr),
            }
            for idx in range(6)
        ]
        bands = {
            band: [float(v.tof_samples - tof_by_band[band][REFERENCE_INDEX].tof_samples) for v in rows]
            for band, rows in tof_by_band.items()
        }
    else:
        delays_by_band = estimate_delays_by_band(
            channels=array_channels,
            sample_rate_hz=sample_rate_hz,
            reference_index=REFERENCE_INDEX,
            max_lag_samples=max_lag_samples,
        )
        chosen = delays_by_band[chosen_band]
        bands = {band: [float(v.lag_samples) for v in rows] for band, rows in delays_by_band.items()}

    measured_lag = np.array([d.lag_samples for d in chosen], dtype=np.float64)
    calibration_delays = (-measured_lag) - tau_geom
    calibration_delays -= calibration_delays[REFERENCE_INDEX]
    out: dict[str, Any] = {
        "azimuth_deg": float(azimuth_deg),
        "role": role,
        "wav_path": str(wav_path),
        "delay_mode": delay_mode,
        "band_used": chosen_band,
        "by_mic": [
            {
                "id": MIC_IDS[idx],
                "lag_samples": float(chosen[idx].lag_samples),
                "corr_peak": float(chosen[idx].peak_signed_corr),
                "delay_samples": float(calibration_delays[idx]),
                "tau_geom_samples": float(tau_geom[idx]),
            }
            for idx in range(6)
        ],
        "bands": bands,
    }
    if absolute_tof_rows is not None:
        out["absolute_tof"] = absolute_tof_rows
        out["max_tof_samples"] = int(max_tof_samples)
    return out


def compile_session(request: CalibrationCompileRequest) -> dict[str, Any]:
    if request.wavefront not in ("spherical", "plane"):
        raise RuntimeError(f"Unsupported wavefront model: {request.wavefront}")
    if request.gain_source not in ("element", "array"):
        raise RuntimeError(f"Unsupported gain source: {request.gain_source}")
    if request.delay_mode not in ("absolute_tof", "relative"):
        raise RuntimeError(f"Unsupported delay mode: {request.delay_mode}")
    if len(request.element_wavs) != 6:
        raise RuntimeError("Expected six element WAV paths")

    geometry = load_geometry(request.geometry)
    array_wav = read_wav(request.array_wav)
    if array_wav.sample_rate_hz <= 0:
        raise RuntimeError("array WAV has invalid sample rate")
    array_channels = split_channels(array_wav, required_channels=6)

    stimulus = None
    if request.stimulus_wav:
        stim_wav = read_wav(request.stimulus_wav)
        if int(stim_wav.sample_rate_hz) != int(array_wav.sample_rate_hz):
            raise RuntimeError("Stimulus and array sample rates must match")
        stimulus = stim_wav.samples[:, 0].copy()
    if request.delay_mode == "absolute_tof" and stimulus is None:
        raise RuntimeError("absolute_tof requires --stimulus-wav / stimulus import")

    max_tof = int(request.max_tof_samples)
    if max_tof <= 0:
        max_tof = default_max_tof_samples(array_wav.sample_rate_hz, distance_m=request.distance_m)

    element_channels: list[np.ndarray] = []
    element_sr = None
    for wav_path in request.element_wavs:
        wav = read_wav(wav_path)
        if wav.channels != 1:
            raise RuntimeError(f"Element WAV must be mono: {wav_path}")
        element_sr = wav.sample_rate_hz if element_sr is None else element_sr
        if wav.sample_rate_hz != element_sr:
            raise RuntimeError("All element WAV files must share sample rate")
        element_channels.append(wav.samples[:, 0].copy())
    if int(element_sr) != int(array_wav.sample_rate_hz):
        raise RuntimeError("Element and array sample rates must match")

    primary_angle = _estimate_array_angle(
        array_channels=array_channels,
        sample_rate_hz=array_wav.sample_rate_hz,
        geometry=geometry,
        azimuth_deg=request.azimuth_deg,
        elevation_deg=request.elevation_deg,
        distance_m=request.distance_m,
        wavefront=request.wavefront,
        max_lag_samples=request.max_lag_samples,
        wav_path=request.array_wav,
        role="primary",
        delay_mode=request.delay_mode,
        stimulus=stimulus,
        max_tof_samples=max_tof,
    )
    calibration_delays = np.array([row["delay_samples"] for row in primary_angle["by_mic"]], dtype=np.float64)

    # Polarity from the same correlation peaks used for delay.
    polarity_meta: list[dict[str, Any]] = []
    for idx, row in enumerate(primary_angle["by_mic"]):
        peak = float(row["corr_peak"])
        decision = polarity_from_delay_peak(
            DelayBandEstimate(lag_samples=float(row["lag_samples"]), peak_abs_corr=abs(peak), peak_signed_corr=peak),
            threshold=request.polarity_threshold,
        )
        polarity_meta.append(
            {
                "id": MIC_IDS[idx],
                "corr_peak": peak,
                "corr_abs": abs(peak),
                "polarity": decision.polarity,
                "ambiguous": decision.ambiguous,
                "reason": decision.reason,
            }
        )
    # Keep runtime polarity baseline as +1 and reserve channel 6 inversion for A/B variants.
    polarity = [1] * 6

    if request.gain_source == "array":
        gains = estimate_gain_linear(array_channels, array_wav.sample_rate_hz, reference_index=REFERENCE_INDEX)
    else:
        gains = estimate_gain_linear(element_channels, array_wav.sample_rate_hz, reference_index=REFERENCE_INDEX)
    gain_linear = [float(g.gain_linear) for g in gains]

    per_mic_eq = _estimate_per_mic_eq(element_channels=element_channels, sample_rate_hz=array_wav.sample_rate_hz)

    rew = None
    common_eq_sections: list[RewFilter] = []
    if request.rew_filter_txt:
        rew = parse_rew_filter_txt(
            request.rew_filter_txt,
            max_q=request.rew_max_q,
            max_boost_db=request.rew_max_boost_db,
            min_freq_hz=request.rew_min_freq_hz,
        )
        common_eq_sections = rew.guarded

    _validate_runtime_bounds(calibration_delays.tolist(), gain_linear, polarity)
    emitted = emit_variants(
        out_dir=request.out_dir,
        tag=request.tag,
        sample_rate_hz=array_wav.sample_rate_hz,
        delays=[float(x) for x in calibration_delays],
        gains=gain_linear,
        ch6_invert=request.ch6_invert_test,
        per_mic_eq=per_mic_eq,
    )

    angle_rows: list[dict[str, Any]] = [primary_angle]
    for azimuth_deg, wav_path in request.extra_array_angles:
        extra_wav = read_wav(wav_path)
        if int(extra_wav.sample_rate_hz) != int(array_wav.sample_rate_hz):
            raise RuntimeError(f"Extra array WAV sample rate mismatch: {wav_path}")
        extra_channels = split_channels(extra_wav, required_channels=6)
        angle_rows.append(
            _estimate_array_angle(
                array_channels=extra_channels,
                sample_rate_hz=array_wav.sample_rate_hz,
                geometry=geometry,
                azimuth_deg=float(azimuth_deg),
                elevation_deg=request.elevation_deg,
                distance_m=request.distance_m,
                wavefront=request.wavefront,
                max_lag_samples=request.max_lag_samples,
                wav_path=Path(wav_path),
                role="extra",
                delay_mode=request.delay_mode,
                stimulus=stimulus,
                max_tof_samples=max_tof,
            )
        )

    report: dict[str, Any] = {
        "sample_rate_hz": array_wav.sample_rate_hz,
        "reference_mic": MIC_IDS[REFERENCE_INDEX],
        "wavefront_model": request.wavefront,
        "primary_azimuth_deg": float(request.azimuth_deg),
        "delay_mode": request.delay_mode,
        "stimulus_wav": str(request.stimulus_wav) if request.stimulus_wav else None,
        "delay": {
            "band_used": primary_angle["band_used"],
            "by_mic": primary_angle["by_mic"],
            "bands": primary_angle["bands"],
            "absolute_tof": primary_angle.get("absolute_tof"),
            "max_tof_samples": primary_angle.get("max_tof_samples"),
        },
        "angles": angle_rows,
        "polarity_detection": polarity_meta,
        "gain": {
            "source": request.gain_source,
            "by_mic": [
                {
                    "id": MIC_IDS[idx],
                    "median_diff_db": float(gains[idx].median_diff_db),
                    "gain_linear": float(gains[idx].gain_linear),
                }
                for idx in range(6)
            ],
        },
        "per_mic_eq": [
            {"id": MIC_IDS[idx], "sections": [sec.__dict__ for sec in per_mic_eq[idx]]}
            for idx in range(6)
        ],
        "common_eq": {
            "enabled_default": False,
            "sections": [sec.__dict__ for sec in common_eq_sections],
        },
        "emitted_yaml": {k: str(v) for k, v in emitted.items()},
        "warnings": [],
    }
    if rew is not None:
        report["common_eq"]["verbatim_sections"] = [sec.__dict__ for sec in rew.verbatim]
        report["common_eq"]["dropped_reasons"] = rew.dropped_reasons
        report["common_eq"]["worst_case_cumulative_boost_db"] = rew.worst_case_cumulative_boost_db
    if np.max(np.abs(np.array(report["delay"]["bands"]["300_3000"]) - np.array(report["delay"]["bands"]["3000_8000"]))) > 1.0:
        report["warnings"].append("band_dependent_lag: low-band and high-band delay estimates differ by >1 sample")
    if any(item["ambiguous"] for item in polarity_meta):
        report["warnings"].append("polarity_unresolved_on_one_or_more_channels")
    if len(angle_rows) > 1:
        report["warnings"].append(
            f"multi_angle: estimated {len(angle_rows)} array azimuths; "
            f"runtime YAML uses primary {request.azimuth_deg:g}° only"
        )

    write_report(request.out_dir, report)
    return report


def main() -> int:
    compile_session(request_from_args(_parse_args()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
