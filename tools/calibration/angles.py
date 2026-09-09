from __future__ import annotations

# Horizontal polar grid used by the characterization protocol (1 m).
# Bypassable in the GUI; 0° is the default runtime-calibration azimuth.
STANDARD_ARRAY_AZIMUTHS_DEG: tuple[float, ...] = (
    0.0,
    30.0,
    60.0,
    90.0,
    120.0,
    140.0,
    160.0,
    180.0,
    -30.0,
    -60.0,
    -90.0,
    -120.0,
    -140.0,
    -160.0,
    -180.0,
)


def format_azimuth_label(azimuth_deg: float) -> str:
    value = float(azimuth_deg)
    if abs(value) < 1.0e-9:
        return "0°"
    if value > 0.0:
        return f"+{value:g}°"
    return f"{value:g}°"


def parse_azimuth_from_name(name: str) -> float | None:
    """Parse trailing azimuth tags like ``array-0deg``, ``+30deg``, ``-90deg``."""
    import re

    match = re.search(r"(?<![A-Za-z0-9])([+-]?\d+(?:\.\d+)?)\s*deg", name, flags=re.IGNORECASE)
    if not match:
        return None
    return float(match.group(1))
