# Linux/Pi Hardware Gate Runbook (Post-PR4)

This runbook captures the remaining on-target validation gates that cannot be fully verified on Windows.

## Scope

Run these gates on Raspberry Pi 5 (or equivalent Linux target) after syncing the integration
branch or merge-commit under validation.

## 0) Linux target setup and baseline build

```bash
sudo apt update
sudo apt install -y cmake ninja-build g++ libasound2-dev libyaml-cpp-dev libspdlog-dev libsamplerate0-dev
cmake --preset default-debug
cmake --build --preset build-debug
ctest --test-dir build --output-on-failure
```

Expected: configure/build/test all pass before hardware gates.

## 1) Drift soak (30 min passthrough stability)

```bash
mkdir -p build/logs
timeout 30m ./build/sonitude_realtime --config config/default.yaml --mode passthrough |& tee build/logs/drift_soak_30m.log
rg -n "XRUN|xrun|recover|asrc|ratio" build/logs/drift_soak_30m.log
```

Record:
- XRUN count and recovery behavior.
- ASRC ratio range over the run.
- Any dropout or restart events.

## 2) ODAS disconnect injection (fallback behavior)

1. Prepare an ODAS-enabled runtime config (temporary copy):

```bash
cp config/default.yaml build/runtime_odas_live.yaml
python3 - <<'PY'
from pathlib import Path
import re
p = Path("build/runtime_odas_live.yaml")
s = p.read_text(encoding="utf-8")
s = re.sub(r"(?m)^\\s*enabled:\\s*false\\s*$", "  enabled: true", s, count=1)
s = re.sub(r"(?m)^\\s*use_mock_provider:\\s*true\\s*$", "  use_mock_provider: false", s, count=1)
p.write_text(s, encoding="utf-8")
print("wrote", p)
PY
```

2. Run realtime and inject disconnect by stopping ODAS for 10s, then restarting:

```bash
./build/sonitude_realtime --config build/runtime_odas_live.yaml --mode beamform |& tee build/logs/odas_disconnect.log
```

In a second shell while realtime is running:

```bash
sudo systemctl stop odas
sleep 10
sudo systemctl start odas
```

3. Verify fallback + recovery logging:

```bash
rg -n "odas|fallback|mock|disconnect|reconnect|timeout|error" build/logs/odas_disconnect.log
```

Record:
- Time to fallback after ODAS loss.
- Whether unsafe states/glitches occurred.
- Time to resume live ODAS tracking after restart.

## 3) Calibration parity (capture -> estimate -> parity check)

```bash
MIN_CORRELATION="<PROJECT_APPROVED_MIN_CORRELATION_PENDING_HARDWARE_CHARACTERIZATION>"
./build/sonitude_calibration_capture --config config/default.yaml --seconds 10 --output build/calibration_capture_pi.wav
./build/sonitude_calibration_estimate \
  --input build/calibration_capture_pi.wav \
  --output build/calibration_estimate_pi.yaml \
  --config config/default.yaml \
  --min-correlation "$MIN_CORRELATION"
```

Threshold selection is pending hardware characterization. Replace the placeholder only with a
project-approved value in `[0, 1]`; this runbook intentionally does not invent an acceptance
threshold. Record the approved value and its characterization evidence with the run.

Compare generated calibration against reference:

```bash
python3 - <<'PY'
import yaml
import math
from pathlib import Path

EXPECTED_IDS = [
    "M0_upper_inner_left",
    "M1_upper_inner_right",
    "M2_upper_outer_left",
    "M3_upper_outer_right",
    "M4_left_earcup",
    "M5_right_earcup",
]

def load_yaml(path):
    return yaml.safe_load(Path(path).read_text(encoding="utf-8"))

def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")

def require_finite(value, label):
    require(isinstance(value, (int, float)) and math.isfinite(float(value)), f"{label} must be finite")

def validate_channels(doc, label):
    require(isinstance(doc, dict), f"{label} must parse as a YAML mapping")
    require("sample_rate_hz" in doc, f"{label} missing sample_rate_hz")
    require_finite(doc["sample_rate_hz"], f"{label}.sample_rate_hz")
    require(int(doc["sample_rate_hz"]) > 0, f"{label}.sample_rate_hz must be > 0")

    channels = doc.get("channels")
    require(isinstance(channels, list), f"{label}.channels must be a YAML sequence")
    require(len(channels) == len(EXPECTED_IDS), f"{label}.channels must contain {len(EXPECTED_IDS)} entries")

    ids = []
    for idx, ch in enumerate(channels):
        require(isinstance(ch, dict), f"{label}.channels[{idx}] must be a mapping")
        channel_id = ch.get("id")
        require(isinstance(channel_id, str) and channel_id, f"{label}.channels[{idx}].id must be a non-empty string")
        ids.append(channel_id)
        require(ch.get("polarity") in (-1, 1), f"{label}.channels[{idx}].polarity must be -1 or +1")
        require_finite(ch.get("delay_samples"), f"{label}.channels[{idx}].delay_samples")
        require_finite(ch.get("gain_linear"), f"{label}.channels[{idx}].gain_linear")
        if "dc_offset" in ch:
            require_finite(ch["dc_offset"], f"{label}.channels[{idx}].dc_offset")

    require(len(set(ids)) == len(ids), f"{label}.channels ids must be unique")
    require(set(ids) == set(EXPECTED_IDS), f"{label}.channels ids must match expected geometry IDs")
    print(f"{label}: sample_rate_hz={int(doc['sample_rate_hz'])}, channels={len(channels)}")

ref = load_yaml("config/calibration_example.yaml")
new = load_yaml("build/calibration_estimate_pi.yaml")
validate_channels(ref, "reference")
validate_channels(new, "estimate")
print("PASS: calibration schema validation checks succeeded")
PY
```

Record:
- Channel-count parity and obvious outlier delays/gains.
- Whether estimated file is valid for runtime load.

### Synthetic-vs-hardware evidence rule

`--synthetic` exists only for deterministic CI/smoke execution checks. Any artifact generated with `--synthetic` must be treated as **non-hardware evidence** and kept separate from Pi acceptance artifacts.

- CI smoke example (non-hardware):

  ```bash
  ./build/sonitude_calibration_capture \
    --synthetic --seconds 2 --output build/ci_capture.wav
  ./build/sonitude_calibration_estimate \
    --synthetic \
    --input build/ci_capture.synthetic.wav \
    --output build/ci_estimate.yaml \
    --config config/default.yaml \
    --min-correlation 0
  ```

  `0` is an explicit testing-path threshold only, not a hardware acceptance threshold.
- Hardware acceptance evidence: run without `--synthetic` on target hardware and archive logs/artifacts with device+config provenance.

## 4) Suppression intelligibility/SNR gate

Render paired outputs from the same input and steering script:

```bash
./build/sonitude_wav_replay --input <six_channel_input.wav> --config config/default.yaml --script <steering.csv> --output build/unsuppressed.wav
./build/sonitude_wav_replay --input <six_channel_input.wav> --config config/default.yaml --script <steering.csv> --output build/suppressed.wav --enable-suppression
```

Minimum objective check (RMS delta sanity):

```bash
python3 - <<'PY'
import wave, struct, math
def rms(path):
    with wave.open(path, "rb") as w:
        raw = w.readframes(w.getnframes())
        if w.getsampwidth() == 2:
            vals = struct.unpack("<" + "h" * (len(raw)//2), raw)
            data = [v/32768.0 for v in vals]
        else:
            vals = struct.unpack("<" + "f" * (len(raw)//4), raw)
            data = list(vals)
    return math.sqrt(sum(x*x for x in data)/max(1, len(data)))
u = rms("build/unsuppressed.wav")
s = rms("build/suppressed.wav")
print(f"unsuppressed_rms={u:.6f}")
print(f"suppressed_rms={s:.6f}")
print(f"rms_ratio={s/max(u,1e-12):.6f}")
PY
```

Record:
- SNR improvement on distractor windows (project metric method).
- Intelligibility impact (STOI/PESQ or project-approved listening protocol).

## 5) Latency validation gate (Milestone 8)

Current local tool status:

```bash
./build/sonitude_latency_marker --help
```

This currently reports placeholder behavior, so final latency acceptance remains pending on:
- Marker generation implementation.
- External analogue loopback capture workflow.
- Reported median/p95/p99 one-way latency with hardware context.

## Exit criteria template

Mark hardware gate complete only when all sections above include:
- exact command history
- captured logs/artifacts paths
- pass/fail decision
- follow-up issue for any failure
