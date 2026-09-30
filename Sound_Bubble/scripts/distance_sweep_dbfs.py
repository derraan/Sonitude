"""
LibriSpeech distance sweep with RT60 overlay for a PyTorch run directory.
"""
from __future__ import annotations

import argparse
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import torch

from src.utils import load_torch_pretrained
from src.validation.distance_sweep_dbfs import (
    DEFAULT_DISTANCES,
    DEFAULT_RT60_LIST,
    DEFAULT_SPEECH_PATH,
    TARGET_IN_DBFS,
    run_distance_sweep,
    write_sweep_csv,
)

COLORS = {
    0.2: "#F58518",
    0.8: "#54A24B",
    1.0: "#B279A2",
    1.6: "#E45756",
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=str, required=True)
    parser.add_argument("--bubble-m", type=float, default=1.5)
    parser.add_argument("--output-prefix", type=str, default=None)
    parser.add_argument("--speech-path", type=str, default=DEFAULT_SPEECH_PATH)
    args = parser.parse_args()

    run_dir = os.path.abspath(args.run_dir)
    bubble_m = float(args.bubble_m)
    out_prefix = args.output_prefix or os.path.join(run_dir, "distance_sweep_dbfs_libri_rt60_overlay")
    out_png = out_prefix + ".png"
    out_csv = out_prefix + ".csv"

    device = "cuda" if torch.cuda.is_available() else "cpu"
    pl = load_torch_pretrained(run_dir, map_location=device)
    model = pl.model.to(device).eval()
    print(f"Loaded {run_dir} epoch={pl.epoch} device={device}")
    print(f"Speech={args.speech_path} RT60s={DEFAULT_RT60_LIST} target_in={TARGET_IN_DBFS} dBFS")

    sweep = run_distance_sweep(
        model,
        device=device,
        speech_path=args.speech_path,
        distances=DEFAULT_DISTANCES,
        rt60_list=DEFAULT_RT60_LIST,
        target_in_dbfs=TARGET_IN_DBFS,
    )
    results = sweep["out_dbfs_by_rt60"]
    in_check = {DEFAULT_RT60_LIST[0]: sweep["input_mic0_dbfs"]}
    distances = sweep["distances_m"]

    for rt60 in DEFAULT_RT60_LIST:
        print(f"\n=== RT60={rt60}s ===")
        for d, out_db, in_db in zip(distances, results[float(rt60)], sweep["input_mic0_dbfs"]):
            print(f"d={d:4.2f}m  in={in_db:7.2f}  out={out_db:7.2f}")

    os.makedirs(os.path.dirname(out_png) or ".", exist_ok=True)
    write_sweep_csv(out_csv, sweep)

    fig, ax = plt.subplots(figsize=(9.0, 5.2), dpi=140)
    ax.plot(
        distances,
        sweep["input_mic0_dbfs"],
        "o-",
        color="#4C78A8",
        label="Input mic0 (level-matched)",
        markersize=3,
        linewidth=1.5,
    )
    for rt60 in DEFAULT_RT60_LIST:
        ax.plot(
            distances,
            results[float(rt60)],
            "s-",
            color=COLORS[rt60],
            label=f"Model Output RT60: {rt60}s",
            markersize=3.5,
            linewidth=1.8,
        )
    ax.axvline(
        bubble_m,
        color="#333333",
        linestyle="--",
        linewidth=2,
        label=f"Bubble radius = {bubble_m:.2f} m",
    )
    ax.set_xlabel("Source distance (m)")
    ax.set_ylabel("Signal level (dBFS)")
    ax.set_title(f"LibriSpeech distance sweep -- RT60 overlay\n{run_dir} (epoch {pl.epoch}, bubble={bubble_m:.2f} m)")
    ax.set_xlim(float(max(distances)), float(min(distances)))
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=9)
    fig.tight_layout()
    fig.savefig(out_png)
    print(f"\nWrote {out_png}")
    print(f"Wrote {out_csv}")


if __name__ == "__main__":
    main()
