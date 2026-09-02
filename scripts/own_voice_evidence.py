#!/usr/bin/env python3
"""Offline own-voice evidence gate (stdlib only).

This tool checks evidence completeness and calculates metrics. It deliberately
contains no performance thresholds: a gate cannot pass until an independently
reviewed threshold file supplies every required limit.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys

SCENARIOS = {
    "silence", "own", "target", "distractor", "own_target",
    "own_distractor", "target_distractor", "own_target_distractor",
    "near_mouth_external", "placement_variation", "contaminated_own_ref",
    "stale_worker", "observation_queue_overflow", "state_queue_overflow",
}
LIMITS = {
    "min_precision", "min_recall", "max_false_positive_rate",
    "max_false_negative_rate", "max_onset_latency_ms",
    "max_release_latency_ms", "min_own_attenuation_db",
    "max_target_attenuation_db", "max_target_distortion",
    "max_processing_p99_ms", "max_queue_drops", "max_xruns",
    "max_end_to_end_latency_ms",
}
TIERS = {
    "A": (1, 1),       # one repaired build, one wearer
    "B": (1, 3),       # proposed minimum for cross-wearer evidence
    "C": (3, 9),       # proposed minimum: 3 builds x 3 wearers
}


def finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[math.ceil(fraction * len(ordered)) - 1]


def load_json(path):
    with pathlib.Path(path).open(encoding="utf-8") as handle:
        return json.load(handle)


def load_jsonl(path):
    rows = []
    with pathlib.Path(path).open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if line.strip():
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as error:
                    raise ValueError(f"invalid JSONL line {line_number}: {error}") from error
    return rows


def validate_manifest(manifest):
    errors = []
    tier = manifest.get("tier")
    if tier not in TIERS:
        errors.append("tier must be A, B, or C")
        return errors
    if manifest.get("channel_count") != 6:
        errors.append("channel_count must be 6")
    for field in ("sample_rate_hz", "geometry_revision", "gain_phase_calibration_revision"):
        if manifest.get(field) in (None, "", 0):
            errors.append(f"{field} is required")
    builds = set(manifest.get("headset_build_ids", []))
    wearers = set(manifest.get("wearer_session_ids", []))
    min_builds, min_wearers = TIERS[tier]
    if len(builds) < min_builds:
        errors.append(f"tier {tier} needs at least {min_builds} headset build(s)")
    if len(wearers) < min_wearers:
        errors.append(f"tier {tier} needs at least {min_wearers} wearer session(s)")
    if tier == "C" and len(wearers) < 3 * len(builds):
        errors.append("tier C needs at least 3 wearer sessions per listed build")
    return errors


def validate_thresholds(thresholds):
    missing = sorted(LIMITS - thresholds.keys())
    invalid = sorted(key for key in LIMITS if key in thresholds and not finite_number(thresholds[key]))
    errors = []
    if missing:
        errors.append("missing thresholds: " + ", ".join(missing))
    if invalid:
        errors.append("thresholds must be finite numbers: " + ", ".join(invalid))
    return errors


def calculate(rows):
    required = {"scenario", "actual_own", "predicted_own", "onset_latency_ms",
                "release_latency_ms", "own_attenuation_db", "target_attenuation_db",
                "target_distortion", "processing_ms", "queue_drops", "xruns",
                "end_to_end_latency_ms"}
    errors = []
    for index, row in enumerate(rows):
        missing = required - row.keys()
        if missing:
            errors.append(f"row {index}: missing {', '.join(sorted(missing))}")
    seen = {row.get("scenario") for row in rows}
    absent = sorted(SCENARIOS - seen)
    if absent:
        errors.append("missing scenarios: " + ", ".join(absent))
    if errors:
        return None, errors

    tp = sum(r["actual_own"] is True and r["predicted_own"] is True for r in rows)
    tn = sum(r["actual_own"] is False and r["predicted_own"] is False for r in rows)
    fp = sum(r["actual_own"] is False and r["predicted_own"] is True for r in rows)
    fn = sum(r["actual_own"] is True and r["predicted_own"] is False for r in rows)
    ratio = lambda n, d: n / d if d else None
    metrics = {
        "precision": ratio(tp, tp + fp), "recall": ratio(tp, tp + fn),
        "false_positive_rate": ratio(fp, fp + tn),
        "false_negative_rate": ratio(fn, fn + tp),
        "onset_latency_ms": max(r["onset_latency_ms"] for r in rows),
        "release_latency_ms": max(r["release_latency_ms"] for r in rows),
        "own_attenuation_db": min(r["own_attenuation_db"] for r in rows),
        "target_attenuation_db": max(r["target_attenuation_db"] for r in rows),
        "target_distortion": max(r["target_distortion"] for r in rows),
        "processing_p99_ms": percentile([r["processing_ms"] for r in rows], .99),
        "queue_drops": sum(r["queue_drops"] for r in rows),
        "xruns": sum(r["xruns"] for r in rows),
        "end_to_end_latency_ms": max(r["end_to_end_latency_ms"] for r in rows),
    }
    if any(value is None or not finite_number(value) for value in metrics.values()):
        errors.append("metrics are incomplete or non-finite")
    return metrics, errors


def decide(metrics, thresholds):
    comparisons = {
        "precision": metrics["precision"] >= thresholds["min_precision"],
        "recall": metrics["recall"] >= thresholds["min_recall"],
        "false_positive_rate": metrics["false_positive_rate"] <= thresholds["max_false_positive_rate"],
        "false_negative_rate": metrics["false_negative_rate"] <= thresholds["max_false_negative_rate"],
        "onset_latency_ms": metrics["onset_latency_ms"] <= thresholds["max_onset_latency_ms"],
        "release_latency_ms": metrics["release_latency_ms"] <= thresholds["max_release_latency_ms"],
        "own_attenuation_db": metrics["own_attenuation_db"] >= thresholds["min_own_attenuation_db"],
        "target_attenuation_db": metrics["target_attenuation_db"] <= thresholds["max_target_attenuation_db"],
        "target_distortion": metrics["target_distortion"] <= thresholds["max_target_distortion"],
        "processing_p99_ms": metrics["processing_p99_ms"] <= thresholds["max_processing_p99_ms"],
        "queue_drops": metrics["queue_drops"] <= thresholds["max_queue_drops"],
        "xruns": metrics["xruns"] <= thresholds["max_xruns"],
        "end_to_end_latency_ms": metrics["end_to_end_latency_ms"] <= thresholds["max_end_to_end_latency_ms"],
    }
    return comparisons, all(comparisons.values())


def self_test():
    manifest = {"tier": "A", "channel_count": 6, "sample_rate_hz": 16000,
                "geometry_revision": "fixture", "gain_phase_calibration_revision": "fixture",
                "headset_build_ids": ["fixture"], "wearer_session_ids": ["fixture"]}
    rows = []
    for scenario in sorted(SCENARIOS):
        own = scenario in {"own", "own_target", "own_distractor", "own_target_distractor"}
        rows.append({"scenario": scenario, "actual_own": own, "predicted_own": own,
                     "onset_latency_ms": 1, "release_latency_ms": 1,
                     "own_attenuation_db": 1, "target_attenuation_db": 0,
                     "target_distortion": 0, "processing_ms": .1,
                     "queue_drops": 0, "xruns": 0, "end_to_end_latency_ms": 1})
    metrics, errors = calculate(rows)
    assert not validate_manifest(manifest) and not errors and metrics["precision"] == 1
    assert validate_thresholds({})
    permissive = {key: 0 for key in LIMITS}
    permissive.update({"min_precision": 0, "min_recall": 0, "min_own_attenuation_db": 0,
                       "max_onset_latency_ms": 10, "max_release_latency_ms": 10,
                       "max_processing_p99_ms": 10, "max_end_to_end_latency_ms": 10})
    assert decide(metrics, permissive)[1]
    broken = rows[:-1]
    assert calculate(broken)[1]
    print("own_voice_evidence self-test passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--manifest")
    parser.add_argument("--results")
    parser.add_argument("--thresholds")
    args = parser.parse_args()
    if args.self_test:
        self_test(); return 0
    if not all((args.manifest, args.results, args.thresholds)):
        parser.error("--manifest, --results, and --thresholds are required")
    manifest, rows, thresholds = load_json(args.manifest), load_jsonl(args.results), load_json(args.thresholds)
    errors = validate_manifest(manifest) + validate_thresholds(thresholds)
    metrics, row_errors = calculate(rows)
    errors += row_errors
    if errors:
        print(json.dumps({"status": "INCOMPLETE", "errors": errors}, indent=2)); return 2
    comparisons, passed = decide(metrics, thresholds)
    print(json.dumps({"status": "PASS" if passed else "FAIL", "metrics": metrics,
                      "comparisons": comparisons}, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
