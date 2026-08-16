# Sonitude Own-Voice Architecture

Status: **ARCHITECTURE READY — OVD PERFORMANCE REQUIRES HARDWARE VALIDATION**

This document is the architecture and implementation gate for Sonitude's own-voice subsystem. It does not claim production OVD accuracy or safe cancellation performance.

## 1. Problem statement

Sonitude must preserve a desired external speaker while independently reducing the wearer's speech and the strongest external distractor, including the simultaneous `own + target + distractor` case. A master-gain reduction during detected own speech is prohibited because it also attenuates the desired speaker.

## 2. Repository findings

This work is stacked on `integration/release-candidate-2026-08-14` / PR #28, not stale `main`. It preserves the integrated runtime invariants:

- `SteeringChannel<RtSteeringSnapshot>` remains the control-to-audio handoff.
- RT payloads remain fixed-size, trivially copyable POD data.
- audio ownership remains `Free -> Producer -> Ready -> Consumer -> Free`.
- RT loops perform no allocation, mutex acquisition, parsing, networking, or logging.
- OVD is orthogonal to ODAS. ODAS tracks external sources; OVD classifies wearer speech.

The current single-reference `SelectiveSuppressor` allocates vectors during configuration and supports only a distractor reference. The new `MultiReferenceSuppressor` uses fixed storage for exactly two interference references and provides explicit adaptation states.

## 3. Scientific basis

OVD is a probabilistic classification problem, not an equality test against one delay, phase, bass band, or immutable transfer vector. Simulated acoustic transfer functions can provide useful spatial/spectral cues, but a 2026 single-microphone study still showed a material simulated-to-real domain gap, so simulation results cannot substitute for Sonitude recordings [1].

Own-voice transfer characteristics can depend on talker and phoneme. Work on occluding in-ear hearables found that speech-dependent RTF models outperformed speech-independent models for that device class [2]. Those body-conduction and occlusion findings do not automatically transfer to Sonitude's external six-microphone geometry.

Multi-microphone/multi-sensor in-ear OVD has been demonstrated with short frame-level processing, but it used an in-ear device with body-vibration sensing and a learned model [3]. It supports the feasibility of bounded online OVD, not a Sonitude accuracy claim.

### Evidence labels

| Statement | Classification |
|---|---|
| Own voice and external voice can exhibit different transfer characteristics | Verified from literature [1], [2] |
| OVD can operate online with bounded frames | Verified for a different in-ear multi-sensor device [3] |
| Sonitude's six microphones contain a stable, discriminative OVTF/RATF | Requires Sonitude measurement |
| Low-frequency body conduction is a strong Sonitude feature | Unverified; requires Sonitude measurement |
| A fixed calibrated spatial filter can create a useful Sonitude `own_ref` | Engineering hypothesis requiring measurement |
| Two-reference NLMS can meet Sonitude target-distortion limits | Engineering hypothesis requiring replay and hardware evidence |

## 4. Canonical signal model

For calibrated microphone (m\),

\[
x_m(f)=H_{t,m}(f)s_t(f)+H_{o,m}(f)s_o(f)+H_{d,m}(f)s_d(f)+v_m(f).
\]

The three references are concurrent views of the same input period:

\[
r_t=W_t^H x, \qquad r_o=W_o^H x, \qquad r_d=W_d^H x.
\]

`target_ref`, `own_ref`, and `distractor_ref` must carry the same capture sequence and timestamp. A component that introduces extra delay must publish that delay and compensate it before multi-reference suppression.

## 5. OVTF/RATF representation

For reference microphone (q\), the measured relative transfer function is

\[
R_{o,m}(f)=\frac{H_{o,m}(f)}{H_{o,q}(f)}.
\]

Level 0 uses fixed-size period summaries: own-to-array RMS, five log RMS ratios, and five normalized correlations. They establish interfaces and queue behavior only.

Level 1 shall use a frequency-dependent statistical model over reliable bins. For feature vector (z\), the implemented detector supports a calibrated diagonal-distance model:

\[
d^2=\frac{\sum_i w_i(z_i-\mu_i)^2\sigma_i^{-2}}{\sum_i w_i},\qquad
p_o=\exp(-d^2/2).
\]

The production feature selection, bin rejection, means, variances, and weights are all `TODO(TBD_FROM_HARDWARE_EVIDENCE)`. A clustered or speech-dependent model may replace the diagonal model without changing the state/channel contracts.

## 6. Own-voice reference extractor

`OwnVoiceReferenceExtractor` is RT DSP. It applies a signed, calibrated six-weight spatial filter to every calibrated microphone frame and emits a continuous PCM reference. Configuration occurs before RT start. The class rejects absent, zero-norm, or non-finite calibration and does not invent fallback weights.

The initial implementation is intentionally deterministic and bounded. The following alternatives remain evidence-driven upgrades: frequency-dependent matched filtering, near-field beamforming, LCMV/MVDR, covariance extraction, or a compact learned extractor.

## 7. OVD architecture

`OwnVoiceFeatureExtractor` runs on the audio side and produces a fixed-size `OwnVoiceObservation`. `OwnVoiceObservationChannel` is bounded SPSC; overflow drops the new observation and increments telemetry. The audio thread never waits.

`OwnVoiceDetector` runs on a SCHED_OTHER worker. It consumes calibrated model statistics, emits a probability, applies activation/release hysteresis, and fails inactive on invalid or stale observations. `OwnVoiceStateChannel` is a second bounded SPSC channel with latest-wins draining at the RT period boundary.

The current code provides the worker-owned processing primitives, not a second lifecycle authority. The existing application lifecycle must own the eventual SCHED_OTHER OVD worker.

## 8. OwnVoiceState semantics

| Field/state | Meaning |
|---|---|
| `probability` | Calibrated model similarity in `[0,1]`; not an accuracy claim |
| `active` | Hysteretic wearer-speech decision |
| `Healthy` | Valid, non-stale observation and usable model |
| `Disabled` | Deliberately disabled; RT cancellation must bypass |
| `ModelUnavailable` | Enabled path lacks a usable calibrated model |
| `InvalidObservation` | Non-finite or contract-invalid input; fail inactive |
| `Stale` | No fresh update within the configured timeout; fail inactive |

## 9. Multi-reference cancellation

The v1 bound is exactly two interference references:

1. wearer own voice;
2. strongest external distractor.

`MultiReferenceSuppressor` maintains two fixed-capacity NLMS filters and exposes four states per reference:

- `ADAPT`: apply the current estimate and update coefficients;
- `HOLD`: apply the current estimate without coefficient updates;
- `DECAY`: apply the estimate while leaking coefficients toward zero;
- `BYPASS`: do not subtract that reference and decay retained coefficients.

No arbitrary-N source machinery is introduced.

## 10. Target protection

Target preservation outranks cancellation depth. The control policy must choose the own-reference adaptation state from:

- fresh and healthy `OwnVoiceState`;
- own-reference quality;
- target activity;
- target/reference correlation;
- filter stability and coefficient limits.

High target activity or correlation with a contaminated `own_ref` must select `HOLD` or `BYPASS`. Stale/disabled/unhealthy OVD must select `BYPASS`. The suppressor also clamps per-sample removal to a configured attenuation floor, but this clamp is not evidence of acceptable target distortion.

`TODO(TBD_FROM_HARDWARE_EVIDENCE)`: define the exact state-transition thresholds and maximum attenuation from labelled `own + target + distractor` replay.

## 11. RT/slow ownership

| Object | Producer/owner | Consumer | Mutation rights | Overflow/failure |
|---|---|---|---|---|
| `OwnVoiceReferenceExtractor` | configured slow; owned RT | RT graph | RT state only after start | zero output + false on contract error |
| `OwnVoiceObservation` | audio RT | OVD worker | immutable after publish | drop newest, count drop |
| `OwnVoiceDetector` | OVD worker | state channel | OVD worker only | invalid/stale -> inactive unhealthy state |
| `OwnVoiceState` | OVD/control | audio RT | immutable after publish | drop publication; next complete state self-heals |
| `MultiReferenceSuppressor` | configured slow; owned RT | output path | RT thread only | bypass/unchanged target on invalid contract |

All queues are constructed before workload start. No OVD object owns the audio deadline.

## 12. ODAS and control interaction

ODAS source tracks must never be re-labelled as wearer identity. During healthy own speech, control should hold the existing external focus, prevent wearer-direction acquisition, continue target rendering, retain distractor suppression, and enable own cancellation only when target-protection evidence permits.

`ConversationState + OwnVoiceState` remains the composition. No `FOCUSED_USER_SPEAKING`-style state explosion is introduced.

## 13. Calibration workflow

Capture synchronized calibrated six-channel PCM for normal/quiet/loud own speech, phonetic diversity, continuous speech, silence, external speech at multiple directions and distances, near-mouth external speech, and environmental noise. Preserve sample rate, geometry revision, gain/phase calibration revision, wearer/session pseudonym, timestamp, and labels.

Offline analysis shall:

1. validate channel synchronization and calibration provenance;
2. frame/STFT the recordings;
3. estimate frequency-dependent RATFs relative to a selected reference microphone;
4. reject low-SNR or low-coherence bins;
5. compare pooled, clustered, and speech-dependent distributions;
6. export a versioned model with feature order, statistics, and calibration revision;
7. reject an enabled runtime configuration if the model is absent or incompatible.

No permanent biometric speaker identity is required.

## 14. Evidence workflow and test matrix

Replay labels must cover silence; own; target; distractor; every two-source combination; simultaneous own + target + distractor; own vocal-level changes; near-field external speech; mouth-direction external speech; placement variation; false positive/negative; stale/stalled worker; queue overflow; and target-contaminated `own_ref`.

Report OVD precision, recall, F1, FP/FN rates, onset/release latency; own-reference own/target and own/distractor ratios; own/distractor attenuation, desired-target attenuation and distortion; processing-time distribution/WCET, queue drops, XRUNs, and end-to-end latency. Overall RMS reduction alone is not an acceptance metric.

The first-syllable limitation is causal. `own_ref` remains continuous; retained stable coefficients, hangover, or weak probability-weighted cancellation may reduce onset leakage. Any lookahead must be included in the latency budget.

## 15. Configuration and failure semantics

`config/own_voice_example.yaml` is intentionally disabled. Every scientific threshold is zero and tagged `TODO(TBD_FROM_HARDWARE_EVIDENCE)`; zero values are not valid for an enabled detector.

Fail-safe rules:

- missing/incompatible calibration: OVD disabled, own cancellation bypassed;
- invalid observation: publish inactive/unhealthy state;
- RT->OVD queue full: drop observation, never block audio;
- stale detector state: inactive/stale, own cancellation bypassed;
- state queue full: retain last state only until its stale deadline;
- suppressor input mismatch/non-finite data: return failure and do not claim valid cancellation;
- ODAS stale while OVD healthy: preserve/hold external focus according to the existing ODAS failsafe, never acquire the wearer;
- disagreement: preserve target and select `HOLD`/`BYPASS`.

## 16. Implementation phases

- Level 0 (this PR): interfaces, bounded queues, continuous fixed-filter reference, deterministic detector contract, two-reference suppressor, tests, architecture.
- Level 1: `TODO(TBD_FROM_HARDWARE_EVIDENCE)` measured Sonitude RATF features/model, offline capture/analyze/replay tools, application worker and RT graph wiring, target-protection policy.
- Level 2: optional compact classifier with the same observation/state interfaces if Level 1 evidence is insufficient.

## 17. Unresolved risks

- M0/M1 absent or intermittent signals make a six-channel transfer model invalid; hardware integrity must be fixed first.
- Channel-level inconsistency and connector intermittency can masquerade as placement or wearer variation.
- A 16 kHz hardware stream processed after resampling at 44.1 kHz cannot recover lost bandwidth and adds latency; evidence must record the true capture rate.
- `own_ref` purity, cross-talk into the target beam, and double-talk convergence are unmeasured.
- The two-reference WCET and scheduling margin require Raspberry Pi measurement.
- Body-conduction usefulness is unknown for Sonitude's microphone placement.

## 18. Architecture decision record

Decision: treat own voice as a first-class continuous PCM reference, classify it on a non-RT worker using a calibrated statistical transfer-feature model, and cancel it as one of exactly two independently controlled interference references. Keep OVD orthogonal to ODAS and conversation state. On uncertainty, staleness, contamination, or model failure, preserve the target and bypass/hold own-voice adaptation.

## References

[1] M. Mayuravaani, W. B. Kleijn, A. Lensen, and C. Sørensen, “Single Microphone Own Voice Detection based on Simulated Transfer Functions for Hearing Aids,” *arXiv preprint arXiv:2603.02724*, 2026. [Online]. Available: https://arxiv.org/abs/2603.02724

[2] M. Ohlenbusch, C. Rollwage, and S. Doclo, “Modeling of speech-dependent own voice transfer characteristics for hearables with an in-ear microphone,” *Acta Acustica*, vol. 8, art. 28, Aug. 2024, doi: 10.1051/aacus/2024032.

[3] P. Pertilä, E. Fagerlund, A. Huttunen, and V. Myllylä, “Online Own Voice Detection for a Multi-channel Multi-sensor In-Ear Device,” *IEEE Sensors Journal*, vol. 21, no. 24, pp. 27686–27697, Dec. 2021, doi: 10.1109/JSEN.2021.3122936.
