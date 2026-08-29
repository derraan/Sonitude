# openMHA Golden Render Validation (M4 and M7)

This runbook records reproducible, out-of-tree reference checks against openMHA without linking or vendoring it into Sonitude.

## Scope guardrails

- No JACK/desktop-audio runtime integration in Sonitude critical path (SCOPE-1).
- No ODAS PCM processing in Sonitude audio path (SCOPE-2).
- No MVDR/GSS/neural adoption in this workflow (SCOPE-3).
- No latency claim updates from this workflow (SCOPE-4).
- No milestone completion without recorded evidence values (SCOPE-6).
- No openMHA source vendoring into this repository (SCOPE-7).

## Inputs

- Six-channel fixture WAV at Sonitude capture rate.
- Runtime YAML aligned with the tested geometry/calibration profile.
- Steering script CSV (`time_seconds,azimuth_deg,elevation_deg`).
- openMHA DS configuration representing the same array geometry.

## Environment

- Linux host with openMHA installed.
- Sonitude built locally with `build/sonitude_wav_replay` available.
- `python3` available for metric extraction.

## Step A: Render Sonitude and openMHA references

1. Export required environment variables:
   - `SONITUDE_REPO=<absolute path to Sonitude>`
   - `OPENMHA_RENDER_CMD=<command that writes OPENMHA_OUTPUT_WAV>`
2. Run:
   - `scripts/openmha_golden_render.sh --input6ch <input.wav> --runtime <runtime.yaml> --steering <script.csv> --sonitude-out <sonitude_m4.wav> --openmha-out <openmha_m4.wav> --metrics-out <m4_metrics.txt>`
3. Confirm that all output files are created.

## Step B: M4 evidence capture (beamformer parity)

Copy the produced metrics into `docs/milestones.md` M4 evidence fields.

Template:

- Fixture file: `<path>`
- Steering script: `<path>`
- openMHA version/tag: `<tag>`
- openMHA cfg path: `<path>`
- `samples_compared`: `<value>`
- `sample_rate_hz`: `<value>`
- `sonitude_rms`: `<value>`
- `openmha_rms`: `<value>`
- `rms_diff`: `<value>`
- `max_abs_sample_diff`: `<value>`
- Result: `pass` or `fail` against project tolerance

## Step C: M7 evidence capture (offline suppressor comparison)

1. Render Sonitude with suppressor enabled:
   - `build/sonitude_wav_replay --input <input.wav> --config <runtime.yaml> --script <script.csv> --output <sonitude_m7.wav> --suppression on`
2. Render openMHA offline reference chain (SCNR/coherence) to `<openmha_m7.wav>`.
3. Compute and record SNR deltas against a shared clean target/reference fixture.

Template:

- Sonitude suppressor config: `enabled=<bool>, fade_ms=<value>, activity_threshold=<value>, confidence_threshold=<value>, ambient_floor_linear=<value>`
- openMHA reference chain: `<cfg or chain id>`
- SNR in/out (Sonitude): `<before dB> -> <after dB>`
- SNR in/out (openMHA reference): `<before dB> -> <after dB>`
- Delta-to-reference: `<dB>`
- Subjective transition artifact check (steering/suppression boundaries): `pass` or `fail`
- Result: `pass` or `fail` against project tolerance

## Notes

- This workflow validates algorithmic parity and bounded behavior only.
- It does not replace M8 on-hardware latency measurement.
