import argparse
import glob
import json
import os
from typing import Dict, List

import numpy as np
import pandas as pd
import torch
import torch.nn.functional as F

from src.metrics.metrics import Metrics
from src.utils import read_audio_file
import src.utils as utils


def _find_sample_dirs(test_dir: str) -> List[str]:
    metadata_paths = glob.glob(os.path.join(test_dir, "**", "metadata.json"), recursive=True)
    return sorted({os.path.dirname(p) for p in metadata_paths})


def load_testcase(sample_dir: str, sr: int, distance_threshold: float, num_slots: int = 2) -> Dict[str, np.ndarray]:
    metadata_path = os.path.join(sample_dir, "metadata.json")
    with open(metadata_path, "rb") as f:
        metadata = json.load(f)

    mixture = read_audio_file(os.path.join(sample_dir, "mixture.wav"), sr).astype(np.float32)
    gt = np.zeros((1, mixture.shape[-1]), dtype=np.float32)

    speakers = [key for key in metadata if key.startswith("voice")]
    inside = []
    for speaker in speakers:
        if metadata["real"]:
            speaker_distance = float(metadata[speaker]["dis"]) / 100.0
        else:
            speaker_distance = float(metadata[speaker]["dis"])
        if speaker_distance <= distance_threshold:
            stem_path = os.path.join(sample_dir, f"mic{0:02d}_{speaker}.wav")
            if not os.path.exists(stem_path):
                continue
            solo = read_audio_file(stem_path, sr).astype(np.float32)
            gt += solo
            inside.append((speaker_distance, solo[0]))

    inside.sort(key=lambda x: x[0])
    spk_distance_m = np.zeros((num_slots,), dtype=np.float32)
    spk_valid = np.zeros((num_slots,), dtype=np.float32)
    spk_ref = np.zeros((num_slots, mixture.shape[-1]), dtype=np.float32)
    for slot, (distance_m, ref_wave) in enumerate(inside[:num_slots]):
        spk_distance_m[slot] = float(distance_m)
        spk_valid[slot] = 1.0
        spk_ref[slot] = ref_wave

    return {
        "mixture": mixture,
        "gt": gt,
        "num_target_speakers": len(inside),
        "num_target_speakers_clamped": int(min(len(inside), 2)),
        "spk_distance_m": spk_distance_m,
        "spk_valid": spk_valid,
        "spk_ref": spk_ref,
    }


def compute_slot_activity_mask(spk_ref: torch.Tensor, spk_valid: torch.Tensor, num_frames: int, vad_threshold_db: float) -> torch.Tensor:
    # spk_ref: [S, T_audio], spk_valid: [S] -> [T_frames, S]
    ratio = 10.0 ** (vad_threshold_db / 10.0)
    power = spk_ref.pow(2).unsqueeze(1)  # [S, 1, T]
    pooled = F.adaptive_avg_pool1d(power, num_frames).squeeze(1)  # [S, T_frames]
    peak = pooled.max(dim=1, keepdim=True).values.clamp_min(1e-8)
    active = pooled > (peak * ratio)
    active = active & (spk_valid.unsqueeze(1) > 0.0)
    return active.transpose(0, 1)


def infer(model, mixture: np.ndarray, device: str):
    with torch.no_grad():
        x = torch.from_numpy(mixture).to(device).unsqueeze(0)
        return model({"mixture": x})


def main(args: argparse.Namespace):
    os.makedirs(args.output_dir, exist_ok=True)
    sample_dirs = _find_sample_dirs(args.test_dir)

    device = "cuda" if args.use_cuda else "cpu"
    baseline_model = utils.load_torch_pretrained(args.baseline_run_dir).model.to(device).eval()
    head_model = utils.load_torch_pretrained(args.head_run_dir).model.to(device).eval()
    si_sdr_i = Metrics("si_sdr_i")

    records = []
    waveform_max_abs_diff = 0.0
    sisdri_abs_diffs = []

    count_frame_correct = 0
    count_frame_total = 0
    count_clip_correct = 0
    slot_distance_errors = []
    false_active_zero_target = 0
    false_count_positive_zero_target = 0
    zero_target_total = 0
    head_available = False

    for sample_dir in sample_dirs:
        sample = load_testcase(sample_dir, args.sr, args.distance_threshold, num_slots=args.num_slots)
        sample_name = os.path.basename(sample_dir)

        baseline_out = infer(baseline_model, sample["mixture"], device)
        head_out = infer(head_model, sample["mixture"], device)

        baseline_audio = baseline_out["output"].detach().cpu()
        head_audio = head_out["output"].detach().cpu()
        gt_t = torch.from_numpy(sample["gt"]).unsqueeze(0)
        mix_t = torch.from_numpy(sample["mixture"][0:1]).unsqueeze(0)
        max_abs = (baseline_audio - head_audio).abs().max().item()
        waveform_max_abs_diff = max(waveform_max_abs_diff, max_abs)

        row = {
            "sample": sample_name,
            "num_target_speakers": int(sample["num_target_speakers"]),
            "num_target_speakers_clamped": int(sample["num_target_speakers_clamped"]),
            "audio_max_abs_diff": max_abs,
        }

        if sample["num_target_speakers"] > 0:
            base_sisdri = si_sdr_i(est=baseline_audio, gt=gt_t, mix=mix_t).item()
            head_sisdri = si_sdr_i(est=head_audio, gt=gt_t, mix=mix_t).item()
            row["baseline_sisdri"] = base_sisdri
            row["head_sisdri"] = head_sisdri
            row["sisdri_delta"] = head_sisdri - base_sisdri
            sisdri_abs_diffs.append(abs(row["sisdri_delta"]))
        else:
            row["baseline_sisdri"] = np.nan
            row["head_sisdri"] = np.nan
            row["sisdri_delta"] = np.nan

        if "speaker_count_logits" in head_out and "spk_distance_m" in head_out:
            head_available = True
            count_logits = head_out["speaker_count_logits"].detach().cpu()[0]
            pred_count_frame = torch.argmax(count_logits, dim=-1)
            gt_count = int(sample["num_target_speakers_clamped"])
            count_frame_correct += int((pred_count_frame == gt_count).sum().item())
            count_frame_total += int(pred_count_frame.numel())
            pred_clip_count = int(torch.mode(pred_count_frame).values.item())
            count_clip_correct += int(pred_clip_count == gt_count)
            row["pred_count_mode"] = pred_clip_count

            pred_dist = head_out["spk_distance_m"].detach().cpu()[0]
            pred_active_logit = head_out.get("spk_active_logit")
            if pred_active_logit is None and "spk_active" in head_out:
                pred_active = head_out["spk_active"].detach().cpu()[0]
                pred_active_logit = torch.log(pred_active.clamp(1e-6, 1.0 - 1e-6)) - torch.log(
                    1.0 - pred_active.clamp(1e-6, 1.0 - 1e-6)
                )
            elif pred_active_logit is not None:
                pred_active_logit = pred_active_logit.detach().cpu()[0]

            if pred_active_logit is not None:
                pred_active_prob = torch.sigmoid(pred_active_logit)
                row["max_spk_active_prob"] = float(pred_active_prob.max().item())
            else:
                pred_active_prob = None
                row["max_spk_active_prob"] = np.nan

            gt_dist = torch.from_numpy(sample["spk_distance_m"])
            gt_valid = torch.from_numpy(sample["spk_valid"])
            gt_ref = torch.from_numpy(sample["spk_ref"])
            activity = compute_slot_activity_mask(
                gt_ref,
                gt_valid,
                pred_dist.shape[0],
                args.vad_threshold_db,
            )
            if activity.any():
                err = (pred_dist - gt_dist.unsqueeze(0))[activity]
                slot_distance_errors.extend(err.numpy().tolist())
                row["slot_distance_mae_active"] = float(err.abs().mean().item())
                row["slot_distance_bias_active"] = float(err.mean().item())
            else:
                row["slot_distance_mae_active"] = np.nan
                row["slot_distance_bias_active"] = np.nan

            if sample["num_target_speakers"] == 0:
                zero_target_total += 1
                if pred_active_prob is not None and bool((pred_active_prob >= args.active_threshold).any().item()):
                    false_active_zero_target += 1
                if bool((pred_count_frame > 0).any().item()):
                    false_count_positive_zero_target += 1

        records.append(row)

    results_df = pd.DataFrame.from_records(records)
    results_df.to_csv(os.path.join(args.output_dir, "speaker_results.csv"), index=False)

    summary = {
        "samples": len(records),
        "count_distance_head_available": bool(head_available),
        "count_frame_accuracy": float(count_frame_correct / count_frame_total) if count_frame_total > 0 else None,
        "count_clip_accuracy": float(count_clip_correct / len(records)) if records else None,
        "slot_distance_metrics_available": bool(head_available and len(slot_distance_errors) > 0),
        "slot_distance_frames_evaluated": len(slot_distance_errors),
        "slot_distance_mae_m": float(np.mean(np.abs(slot_distance_errors))) if slot_distance_errors else None,
        "slot_distance_rmse_m": float(np.sqrt(np.mean(np.square(slot_distance_errors)))) if slot_distance_errors else None,
        "slot_distance_bias_m": float(np.mean(slot_distance_errors)) if slot_distance_errors else None,
        "false_active_rate_zero_target": (
            float(false_active_zero_target / zero_target_total) if zero_target_total > 0 else None
        ),
        "false_count_positive_rate_zero_target": (
            float(false_count_positive_zero_target / zero_target_total) if zero_target_total > 0 else None
        ),
        "max_audio_waveform_abs_diff": float(waveform_max_abs_diff),
        "mean_abs_sisdri_delta_db": float(np.mean(sisdri_abs_diffs)) if sisdri_abs_diffs else None,
        "max_abs_sisdri_delta_db": float(np.max(sisdri_abs_diffs)) if sisdri_abs_diffs else None,
        "baseline_run_dir": os.path.abspath(args.baseline_run_dir),
        "head_run_dir": os.path.abspath(args.head_run_dir),
        "distance_threshold": float(args.distance_threshold),
        "vad_threshold_db": float(args.vad_threshold_db),
        "active_threshold": float(args.active_threshold),
        "num_slots": int(args.num_slots),
    }

    with open(os.path.join(args.output_dir, "speaker_summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    with open(os.path.join(args.output_dir, "args.json"), "w", encoding="utf-8") as f:
        json.dump(vars(args), f, indent=2)

    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("test_dir", type=str, help="Path to syn_test root directory")
    parser.add_argument("baseline_run_dir", type=str, help="Run dir of frozen backbone checkpoint")
    parser.add_argument("head_run_dir", type=str, help="Run dir of speaker-head checkpoint")
    parser.add_argument("output_dir", type=str, help="Output directory")
    parser.add_argument("--distance_threshold", type=float, default=1.5)
    parser.add_argument("--sr", type=int, default=24000)
    parser.add_argument("--vad_threshold_db", type=float, default=-35.0)
    parser.add_argument("--active_threshold", type=float, default=0.5)
    parser.add_argument("--num_slots", type=int, default=2)
    parser.add_argument("--use_cuda", action="store_true")
    main(parser.parse_args())
