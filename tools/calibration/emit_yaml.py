from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from .geometry import MIC_IDS
from .rew_filters import RewFilter


@dataclass(frozen=True)
class CalibrationVectors:
    delay_samples: list[float]
    gain_linear: list[float]
    polarity: list[int]


def _section_dict(section: RewFilter) -> dict[str, Any]:
    return {
        "type": section.ftype,
        "freq_hz": float(section.freq_hz),
        "gain_db": float(section.gain_db),
        "q": float(section.q),
    }


def _channel_entry(
    mic_id: str,
    delay_samples: float,
    gain_linear: float,
    polarity: int,
    eq_sections: list[RewFilter] | None = None,
) -> dict[str, Any]:
    out: dict[str, Any] = {
        "id": mic_id,
        "polarity": int(polarity),
        "gain_linear": float(gain_linear),
        "delay_samples": float(delay_samples),
        "dc_offset": 0.0,
    }
    if eq_sections is not None:
        out["eq"] = {
            "enabled": False,
            "sections": [_section_dict(sec) for sec in eq_sections],
        }
    return out


def _base_doc(sample_rate_hz: int, channels: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "sample_rate_hz": int(sample_rate_hz),
        "channels": channels,
    }


def emit_variants(
    out_dir: str | Path,
    tag: str,
    sample_rate_hz: int,
    delays: list[float],
    gains: list[float],
    ch6_invert: bool,
    per_mic_eq: list[list[RewFilter]] | None = None,
) -> dict[str, Path]:
    outp = Path(out_dir)
    outp.mkdir(parents=True, exist_ok=True)
    if len(delays) != 6 or len(gains) != 6:
        raise RuntimeError("Expected six delay and gain values")
    if per_mic_eq is not None and len(per_mic_eq) != 6:
        raise RuntimeError("Expected six per-mic EQ lists")

    identity_polarity = [1] * 6
    with_ch6_test = [1, 1, 1, 1, 1, (-1 if ch6_invert else 1)]

    variants = {
        "A_baseline": ([(0.0, 1.0, p) for p in identity_polarity]),
        "B_delay": ([(delays[i], 1.0, identity_polarity[i]) for i in range(6)]),
        "C_polarity": ([(0.0, 1.0, with_ch6_test[i]) for i in range(6)]),
        "D_delay_polarity": ([(delays[i], 1.0, with_ch6_test[i]) for i in range(6)]),
        "E_full": ([(delays[i], gains[i], with_ch6_test[i]) for i in range(6)]),
    }

    emitted: dict[str, Path] = {}
    for variant_name, rows in variants.items():
        channels: list[dict[str, Any]] = []
        for idx, (delay, gain, pol) in enumerate(rows):
            eq_rows = None if per_mic_eq is None else per_mic_eq[idx]
            channels.append(_channel_entry(MIC_IDS[idx], delay, gain, pol, eq_rows))
        doc = _base_doc(sample_rate_hz=sample_rate_hz, channels=channels)
        dst = outp / f"calibration_{tag}_{variant_name}.yaml"
        dst.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")
        emitted[variant_name] = dst
    return emitted
