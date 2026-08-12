#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/openmha_golden_render.sh \
    --input6ch <input_6ch_wav> \
    --runtime <runtime_yaml> \
    --steering <steering_csv> \
    --sonitude-out <sonitude_mono_wav> \
    --openmha-out <openmha_mono_wav> \
    --metrics-out <metrics_txt>

Required environment variables:
  SONITUDE_REPO        Absolute path to Sonitude repository.
  OPENMHA_RENDER_CMD   Shell command that renders openMHA output WAV.
                       It must write output to the path passed as:
                       OPENMHA_OUTPUT_WAV=<path>

Example:
  export SONITUDE_REPO=/home/pi/Sonitude
  export OPENMHA_RENDER_CMD='mha -q ?read:cfg/m4_delaysum.cfg'
  OPENMHA_OUTPUT_WAV=/tmp/openmha.wav eval "$OPENMHA_RENDER_CMD"
EOF
}

INPUT6=""
RUNTIME=""
STEERING=""
SONITUDE_OUT=""
OPENMHA_OUT=""
METRICS_OUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input6ch) INPUT6="${2:-}"; shift 2 ;;
    --runtime) RUNTIME="${2:-}"; shift 2 ;;
    --steering) STEERING="${2:-}"; shift 2 ;;
    --sonitude-out) SONITUDE_OUT="${2:-}"; shift 2 ;;
    --openmha-out) OPENMHA_OUT="${2:-}"; shift 2 ;;
    --metrics-out) METRICS_OUT="${2:-}"; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "${SONITUDE_REPO:-}" || -z "${OPENMHA_RENDER_CMD:-}" ]]; then
  echo "SONITUDE_REPO and OPENMHA_RENDER_CMD must be set." >&2
  usage
  exit 2
fi

if [[ -z "$INPUT6" || -z "$RUNTIME" || -z "$STEERING" || -z "$SONITUDE_OUT" || -z "$OPENMHA_OUT" || -z "$METRICS_OUT" ]]; then
  usage
  exit 2
fi

echo "[1/3] Rendering Sonitude beamformed output..."
"$SONITUDE_REPO/build/sonitude_wav_replay" \
  --input "$INPUT6" \
  --config "$RUNTIME" \
  --script "$STEERING" \
  --output "$SONITUDE_OUT"

echo "[2/3] Rendering openMHA golden output..."
OPENMHA_OUTPUT_WAV="$OPENMHA_OUT" eval "$OPENMHA_RENDER_CMD"
if [[ ! -f "$OPENMHA_OUT" ]]; then
  echo "openMHA output was not created: $OPENMHA_OUT" >&2
  exit 1
fi

echo "[3/3] Computing RMS and max-sample deltas..."
python3 - "$SONITUDE_OUT" "$OPENMHA_OUT" "$METRICS_OUT" <<'PY'
import math
import struct
import sys
import wave

soni_path, openmha_path, metrics_path = sys.argv[1:4]

def read_mono(path: str):
    with wave.open(path, "rb") as w:
        channels = w.getnchannels()
        rate = w.getframerate()
        width = w.getsampwidth()
        frames = w.getnframes()
        raw = w.readframes(frames)
    if channels != 1:
        raise RuntimeError(f"{path} is not mono (channels={channels})")
    if width == 2:
        vals = struct.unpack("<" + "h" * frames, raw)
        data = [v / 32768.0 for v in vals]
    elif width == 4:
        vals = struct.unpack("<" + "f" * frames, raw)
        data = list(vals)
    else:
        raise RuntimeError(f"Unsupported sample width in {path}: {width}")
    return rate, data

rate_a, a = read_mono(soni_path)
rate_b, b = read_mono(openmha_path)
if rate_a != rate_b:
    raise RuntimeError(f"Sample rate mismatch: {rate_a} vs {rate_b}")

n = min(len(a), len(b))
if n == 0:
    raise RuntimeError("No samples available for comparison")
a = a[:n]
b = b[:n]

diff = [x - y for x, y in zip(a, b)]
rms_a = math.sqrt(sum(x * x for x in a) / n)
rms_b = math.sqrt(sum(x * x for x in b) / n)
rms_diff = math.sqrt(sum(d * d for d in diff) / n)
max_abs_diff = max(abs(d) for d in diff)

with open(metrics_path, "w", encoding="utf-8") as f:
    f.write("openMHA M4 golden-render metrics\n")
    f.write(f"samples_compared={n}\n")
    f.write(f"sample_rate_hz={rate_a}\n")
    f.write(f"sonitude_rms={rms_a:.8f}\n")
    f.write(f"openmha_rms={rms_b:.8f}\n")
    f.write(f"rms_diff={rms_diff:.8f}\n")
    f.write(f"max_abs_sample_diff={max_abs_diff:.8f}\n")

print(f"Wrote metrics to {metrics_path}")
PY

echo "Done."
