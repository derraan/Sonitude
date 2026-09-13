from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf

from .metrics import pair_metrics


@dataclass
class SampleResult:
    id: str
    snr_baseline_db: float
    snr_candidate_db: float
    snri_baseline_db: float
    snri_candidate_db: float
    sisdr_baseline_db: float
    sisdr_candidate_db: float
    sisdri_baseline_db: float
    sisdri_candidate_db: float
    pesq_baseline: float | None
    pesq_candidate: float | None
    stoi_baseline: float | None
    stoi_candidate: float | None


def _load_audio(path: Path) -> tuple[np.ndarray, int]:
    x, sr = sf.read(str(path), dtype="float32")
    if x.ndim > 1:
        x = np.mean(x, axis=1)
    return x.astype(np.float32), int(sr)


def _resolve(root: Path, rel_or_abs: str) -> Path:
    p = Path(rel_or_abs)
    return p if p.is_absolute() else (root / p)


def _try_optional_metrics():
    pesq_fn = None
    stoi_fn = None
    try:
        from pesq import pesq as pesq_fn_local  # type: ignore

        pesq_fn = pesq_fn_local
    except Exception:
        pesq_fn = None
    try:
        from pystoi import stoi as stoi_fn_local  # type: ignore

        stoi_fn = stoi_fn_local
    except Exception:
        stoi_fn = None
    return pesq_fn, stoi_fn


def evaluate_manifest(
    manifest_path: Path,
    candidate_key: str,
    baseline_key: str,
    include_optional: bool,
) -> dict[str, Any]:
    payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    root = manifest_path.parent
    entries = payload.get("samples", [])
    if not entries:
        raise ValueError("Manifest has no samples")

    pesq_fn, stoi_fn = _try_optional_metrics()
    if include_optional and (pesq_fn is None or stoi_fn is None):
        print("Optional PESQ/STOI packages not found; continuing with SNR/SI-SDR metrics only.")

    sample_rows: list[SampleResult] = []
    for i, s in enumerate(entries):
        sample_id = str(s.get("id", f"sample_{i:05d}"))
        ref_path = _resolve(root, s["target"])
        mix_path = _resolve(root, s["mixture"])
        base_path = _resolve(root, s[baseline_key])
        cand_path = _resolve(root, s[candidate_key])

        ref, ref_sr = _load_audio(ref_path)
        mix, mix_sr = _load_audio(mix_path)
        base, base_sr = _load_audio(base_path)
        cand, cand_sr = _load_audio(cand_path)
        if len({ref_sr, mix_sr, base_sr, cand_sr}) != 1:
            raise ValueError(f"Sample {sample_id} has mismatched sample rates")

        n = min(ref.shape[0], mix.shape[0], base.shape[0], cand.shape[0])
        ref = ref[:n]
        mix = mix[:n]
        base = base[:n]
        cand = cand[:n]

        mix_m = pair_metrics(ref, mix)
        base_m = pair_metrics(ref, base)
        cand_m = pair_metrics(ref, cand)

        pesq_base = None
        pesq_cand = None
        stoi_base = None
        stoi_cand = None
        if include_optional and pesq_fn is not None and stoi_fn is not None:
            mode = "wb" if ref_sr >= 16000 else "nb"
            pesq_base = float(pesq_fn(ref_sr, ref, base, mode))
            pesq_cand = float(pesq_fn(ref_sr, ref, cand, mode))
            stoi_base = float(stoi_fn(ref, base, ref_sr, extended=False))
            stoi_cand = float(stoi_fn(ref, cand, ref_sr, extended=False))

        sample_rows.append(
            SampleResult(
                id=sample_id,
                snr_baseline_db=base_m.snr_db,
                snr_candidate_db=cand_m.snr_db,
                snri_baseline_db=base_m.snr_db - mix_m.snr_db,
                snri_candidate_db=cand_m.snr_db - mix_m.snr_db,
                sisdr_baseline_db=base_m.si_sdr_db,
                sisdr_candidate_db=cand_m.si_sdr_db,
                sisdri_baseline_db=base_m.si_sdr_db - mix_m.si_sdr_db,
                sisdri_candidate_db=cand_m.si_sdr_db - mix_m.si_sdr_db,
                pesq_baseline=pesq_base,
                pesq_candidate=pesq_cand,
                stoi_baseline=stoi_base,
                stoi_candidate=stoi_cand,
            )
        )

    def _mean(key: str) -> float | None:
        vals = [getattr(r, key) for r in sample_rows if getattr(r, key) is not None]
        if not vals:
            return None
        return float(np.mean(np.asarray(vals, dtype=np.float64)))

    summary = {
        "num_samples": len(sample_rows),
        "mean_snri_baseline_db": _mean("snri_baseline_db"),
        "mean_snri_candidate_db": _mean("snri_candidate_db"),
        "mean_sisdri_baseline_db": _mean("sisdri_baseline_db"),
        "mean_sisdri_candidate_db": _mean("sisdri_candidate_db"),
        "mean_pesq_baseline": _mean("pesq_baseline"),
        "mean_pesq_candidate": _mean("pesq_candidate"),
        "mean_stoi_baseline": _mean("stoi_baseline"),
        "mean_stoi_candidate": _mean("stoi_candidate"),
    }
    return {"summary": summary, "samples": [asdict(r) for r in sample_rows]}


def main() -> None:
    parser = argparse.ArgumentParser(description="Evaluate DSENet candidate vs baseline from dataset manifest.")
    parser.add_argument("--manifest", type=Path, required=True, help="JSON with samples list")
    parser.add_argument("--candidate-key", default="candidate", help="Manifest key name for candidate audio path")
    parser.add_argument("--baseline-key", default="baseline", help="Manifest key name for baseline audio path")
    parser.add_argument("--include-optional", action="store_true", help="Try PESQ/STOI if installed")
    parser.add_argument("--out-json", type=Path, required=True)
    args = parser.parse_args()

    report = evaluate_manifest(
        manifest_path=args.manifest,
        candidate_key=args.candidate_key,
        baseline_key=args.baseline_key,
        include_optional=args.include_optional,
    )
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))
    print(f"Wrote report: {args.out_json}")


if __name__ == "__main__":
    main()
