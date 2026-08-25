# Own-voice calibration, replay, and runtime-enable gate

Status: **PROPOSED EVIDENCE CONTRACT — PERFORMANCE LIMITS UNSET**

This document separates code that can be reviewed now from behavior that cannot be
enabled until Sonitude hardware evidence exists. It does not claim OVD accuracy,
cancellation depth, acceptable target distortion, or Raspberry Pi timing margin.

## Ready for implementation review

The following Level-0 contracts can be accepted independently of acoustic performance:

- fixed-size, trivially-copyable observation/state payloads;
- bounded SPSC channels with non-blocking overflow behavior;
- continuous six-input fixed-filter reference extraction with atomic failure;
- deterministic diagonal-distance scoring and hysteresis semantics;
- disabled, unavailable, invalid, and stale fail-inactive states;
- two fixed-storage NLMS references with ADAPT/HOLD/DECAY/BYPASS;
- atomic bypass on invalid suppressor input and bounded coefficients;
- telemetry field plumbing and an intentionally disabled example configuration.

These tests establish software contracts only. Synthetic correlated signals do not
establish acoustic separation, target safety, or real-time performance.

## Evidence-gated work

The following remain `TODO(TBD_FROM_HARDWARE_EVIDENCE)`:

- repair and verify all six capture channels, including M0/M1;
- measure gain/phase calibration and channel synchronization;
- select RATF/OVTF features, reliable bins, model statistics, and spatial weights;
- choose activation/release thresholds, hold times, and stale timeout;
- choose NLMS taps, step size, leakage, coefficient limit, mix, and maximum attenuation;
- define target-activity/correlation protection transitions;
- measure Pi processing time, XRUNs, queue behavior, and end-to-end latency;
- wire the worker and RT graph; and
- set `own_voice.enabled` or cancellation enabled to true.

## Cumulative qualification tiers

Counts below are proposed minimum test coverage, not statistical performance claims.

| Gate | Hardware/people | What it permits |
|---|---|---|
| A — mechanism | 1 repaired build, 1 wearer session | Offline replay development only; runtime stays disabled |
| B — wearer | Same build, at least 3 independent wearer sessions | A candidate pooled/clustered model; runtime stays disabled |
| C — build | At least 3 builds and 3 wearer sessions per build | Eligibility for runtime-integration review, not automatic enablement |

Training/calibration sessions must not also be the acceptance sessions. Report results
per wearer and per build; do not accept only a favorable pooled average.

## Smallest replay matrix

Each tier must contain every row. A row may contain several labelled periods, but no
single period may stand in for incompatible conditions.

| Scenario | OVD check | Cancellation/target check |
|---|---|---|
| silence | false activation | bypass stability |
| own | detection, onset/release | own attenuation |
| target | own-voice false positive | target attenuation/distortion |
| distractor | own-voice false positive | distractor path independence |
| own + target | double-talk | own attenuation and target preservation |
| own + distractor | discrimination | both reference controls |
| target + distractor | false activation | target preservation |
| own + target + distractor | full concurrency | target-first behavior |
| near-mouth external speech | near-field false positive | bypass own cancellation |
| placement variation | model robustness | safe degradation |
| target-contaminated own_ref | detector/control disagreement | HOLD or BYPASS |
| stalled OVD worker | staleness | inactive and BYPASS |
| observation queue overflow | observable drop | audio never blocks |
| state queue overflow | latest complete state | stale deadline still applies |

Own-speech rows must include quiet, normal, and loud speech plus varied phonetic content.
External speech must include more than one direction and distance. These are within-row
conditions, not extra headline scenarios.

## Required provenance

Every replay set records the raw synchronized six-channel PCM hash, true capture sample
rate, geometry/build revision, gain/phase calibration revision, anonymized wearer/session,
placement, source geometry, labels, calibration/model revision, code commit, configuration,
and timestamps. Resampling does not change the declared hardware capture bandwidth.

## Metrics and acceptance rule

The replay tool must report precision, recall, false-positive and false-negative rates,
worst observed onset/release latency, minimum own attenuation, maximum target attenuation
and target distortion, processing p99, queue drops, XRUNs, and worst end-to-end latency.
Overall RMS reduction is not an acceptance metric.

All limits live in a reviewed threshold file. There are deliberately no defaults. Missing,
null, non-finite, or unevaluated limits produce `INCOMPLETE`, never `PASS`. Thresholds must
be agreed before inspecting the held-out acceptance results.

Run the stdlib-only scaffold with:

```sh
python3 scripts/own_voice_evidence.py --self-test
python3 scripts/own_voice_evidence.py \
  --manifest evidence/own_voice/manifest.json \
  --results evidence/own_voice/results.jsonl \
  --thresholds evidence/own_voice/thresholds.json
```

## Runtime-enable rule

Runtime integration may be reviewed only after Gate C passes on held-out data and Pi timing
evidence passes. Enabling cancellation additionally requires the target-protection rows to
pass. A compatible signed/versioned calibration and model must be present. Any missing or
incompatible artifact, stale/unhealthy OVD state, contaminated reference, or failed safety
comparison keeps own cancellation in BYPASS.

Gate passage permits review; it does not by itself change configuration. The enable change
must be a separate commit with the evidence artifact identifiers in its review record.
