from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import yaml


MIC_IDS = [
    "M0_left_ear",
    "M1_left_arc",
    "M2_left_top",
    "M3_right_top",
    "M4_right_arc",
    "M5_right_ear",
]
REFERENCE_INDEX = 2
SPEED_OF_SOUND_MPS = 343.0


@dataclass(frozen=True)
class Geometry:
    profile_name: str
    mic_ids: list[str]
    xyz_m: np.ndarray  # shape [6, 3]


def load_geometry(path: str | Path) -> Geometry:
    with open(path, "r", encoding="utf-8") as handle:
        root = yaml.safe_load(handle)
    profile_name = str(root.get("profile_name", ""))
    mics = root.get("microphones")
    if not isinstance(mics, list) or len(mics) != 6:
        raise RuntimeError("Geometry must contain exactly six microphones")

    ids: list[str] = []
    xyz = np.zeros((6, 3), dtype=np.float64)
    for idx, mic in enumerate(mics):
        mid = str(mic["id"])
        ids.append(mid)
        xyz[idx, 0] = float(mic["x"])
        xyz[idx, 1] = float(mic["y"])
        xyz[idx, 2] = float(mic["z"])

    if ids != MIC_IDS:
        raise RuntimeError(f"Geometry microphone IDs must match expected order: {MIC_IDS}")
    return Geometry(profile_name=profile_name, mic_ids=ids, xyz_m=xyz)


def unit_vector_from_az_el_deg(azimuth_deg: float, elevation_deg: float) -> np.ndarray:
    """Head-frame unit vector: az=0 → +Y (forward), +az → +X (listener-right)."""
    az = np.deg2rad(azimuth_deg)
    el = np.deg2rad(elevation_deg)
    cos_el = np.cos(el)
    return np.array(
        [
            cos_el * np.sin(az),
            cos_el * np.cos(az),
            np.sin(el),
        ],
        dtype=np.float64,
    )


def geometric_delay_samples_plane(
    geometry: Geometry,
    sample_rate_hz: int,
    azimuth_deg: float,
    elevation_deg: float,
    speed_of_sound_mps: float = SPEED_OF_SOUND_MPS,
) -> np.ndarray:
    # Mirror beamformer.cpp: tau = -((dot_i - dot_ref) / c)
    u = unit_vector_from_az_el_deg(azimuth_deg=azimuth_deg, elevation_deg=elevation_deg)
    dots = geometry.xyz_m @ u
    ref_dot = dots[REFERENCE_INDEX]
    tau_sec = -((dots - ref_dot) / speed_of_sound_mps)
    return tau_sec * float(sample_rate_hz)


def geometric_delay_samples_spherical(
    geometry: Geometry,
    sample_rate_hz: int,
    azimuth_deg: float,
    elevation_deg: float,
    distance_m: float,
    speed_of_sound_mps: float = SPEED_OF_SOUND_MPS,
) -> np.ndarray:
    u = unit_vector_from_az_el_deg(azimuth_deg=azimuth_deg, elevation_deg=elevation_deg)
    source_xyz = u * float(distance_m)
    ranges = np.linalg.norm(source_xyz[None, :] - geometry.xyz_m, axis=1)
    ref_range = ranges[REFERENCE_INDEX]
    tau_sec = (ranges - ref_range) / speed_of_sound_mps
    return tau_sec * float(sample_rate_hz)
