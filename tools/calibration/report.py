from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .geometry import MIC_IDS


def write_report(out_dir: str | Path, payload: dict[str, Any]) -> tuple[Path, Path]:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    json_path = out / "calibration_report.json"
    md_path = out / "calibration_report.md"
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    lines: list[str] = []
    lines.append("# Calibration report")
    lines.append("")
    lines.append(f"- sample_rate_hz: {payload['sample_rate_hz']}")
    lines.append(f"- reference_mic: {payload['reference_mic']}")
    lines.append(f"- wavefront_model: {payload['wavefront_model']}")
    lines.append("")
    lines.append("## Delay estimate (300-8000 Hz)")
    lines.append("")
    lines.append("| Mic | lag_samples | delay_samples | corr | polarity |")
    lines.append("| --- | ---: | ---: | ---: | ---: |")
    polarity_by_id = {row["id"]: row["polarity"] for row in payload.get("polarity_detection", [])}
    for idx, row in enumerate(payload["delay"]["by_mic"]):
        pol = polarity_by_id.get(row["id"], 1)
        lines.append(
            f"| {MIC_IDS[idx]} | {row['lag_samples']:.3f} | {row['delay_samples']:.3f} | "
            f"{row['corr_peak']:.3f} | {pol} |"
        )
    lines.append("")
    lines.append("## Gain estimate")
    lines.append("")
    lines.append("| Mic | median_diff_db | gain_linear |")
    lines.append("| --- | ---: | ---: |")
    for idx, row in enumerate(payload["gain"]["by_mic"]):
        lines.append(f"| {MIC_IDS[idx]} | {row['median_diff_db']:.3f} | {row['gain_linear']:.5f} |")
    lines.append("")
    lines.append("## Warnings")
    lines.append("")
    for warning in payload.get("warnings", []):
        lines.append(f"- {warning}")
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return json_path, md_path
