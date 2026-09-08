#!/usr/bin/env bash
# =============================================================================
# bootstrap.sh — Sound Bubble Pi 5 clean-slate setup + pipeline launcher
# =============================================================================
# Run once on a fresh Pi OS install (no reflash needed):
#   bash bootstrap.sh
#
# To just run the pipeline after setup:
#   bash bootstrap.sh --run-only
#
# PREEMPT_RT follow-up (optional, not part of this bootstrap):
#   See by/RT-Kernel for Raspberry Pi 5 RT kernel build/install instructions:
#   https://github.com/by/RT-Kernel
# =============================================================================

set -euo pipefail

# =============================================================================
# USER CONFIG — edit these before running
# =============================================================================

# --- Audio devices ---
# Run: python -c "import sounddevice as sd; print(sd.query_devices())"
# to find your device indices after setup.
INPUT_DEVICE=0          # INMP441 8ch mic array index
OUTPUT_DEVICE=1         # USB headphone / speaker index

# --- Model selection ---
USE_ZOO=true            # true = use zoo dir  |  false = use single --model
ZOO_DIR="edge/zoo"      # relative to project dir; used when USE_ZOO=true
BUBBLE_RADIUS=1.5       # options: 1.0, 1.5, 2.0

# Single-model fallback (only used when USE_ZOO=false)
MODEL_PATH="edge/model.onnx"
CONTRACT_PATH="edge/model.runtime.json"

# --- Audio config ---
INPUT_CHANNELS=8
OUTPUT_CHANNELS=2
CHANNEL_MAP="0,1,2,3,4,5"
INPUT_SR=44100
MODEL_SR=24000
OUTPUT_SR=48000

# --- ONNX Runtime threading ---
# Pi 5 has 4 cores. Keep intra <= 3 to leave one core for PortAudio callback.
INTRA_OP_THREADS=3
INTER_OP_THREADS=1

# --- Pipeline tuning ---
HOPS_PER_IO=8           # ring buffer depth: 4=lowest latency, 8=jitter-tolerant
RT_FIFO_PRIORITY=50     # SCHED_FIFO priority (0 = disable RT scheduling)
MIX_DRY=0.0             # 0.0 = full model output | 1.0 = passthrough
SILENCE_DBFS=-50.0      # silence gate threshold; use -200 if mics are dead
MAX_STEPS_PER_BLOCK=32  # inference catch-up cap per capture block

# =============================================================================
# INTERNALS — no need to edit below this line
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
VENV_DIR="$PROJECT_DIR/.venv"
VENV_PY="$VENV_DIR/bin/python"

RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'
info()  { echo -e "${GREEN}[info]${NC} $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC} $*"; }
error() { echo -e "${RED}[error]${NC} $*"; exit 1; }

RUN_ONLY=false
for arg in "$@"; do [[ "$arg" == "--run-only" ]] && RUN_ONLY=true; done

# =============================================================================
# SETUP (skipped with --run-only)
# =============================================================================

if [ "$RUN_ONLY" = false ]; then

  echo ""
  echo "================================================================="
  echo "  Sound Bubble — Pi 5 Bootstrap"
  echo "================================================================="
  echo ""

  # --- 1) Nuke stale venvs and user pip installs ---
  info "[1/6] Cleaning stale venvs and user pip installs..."

  # Remove any venv inside project dir
  if [ -e "$VENV_DIR" ]; then
    rm -rf "$VENV_DIR"
    info "  Removed $VENV_DIR"
  fi

  # Remove any stale ~/.venv (previous failed setup)
  if [ -d "$HOME/.venv" ]; then
    rm -rf "$HOME/.venv"
    info "  Removed ~/.venv"
  fi

  # Remove user pip installs (pip install --user outside venv)
  if [ -d "$HOME/.local/lib" ]; then
    rm -rf "$HOME/.local/lib/python"*/site-packages 2>/dev/null || true
    info "  Cleared ~/.local/lib/python*/site-packages"
  fi

  # Verify system Python is clean
  if python3 -c "import numpy" 2>/dev/null; then
    warn "  numpy still importable from system Python — may be system package"
    warn "  If issues persist: sudo apt remove python3-numpy"
  else
    info "  System Python clean (numpy not found — expected)"
  fi

  # --- 2) System packages ---
  info "[2/6] Installing system packages..."
  sudo apt update -qq
  sudo apt install -y -qq \
    git python3-pip python3-venv \
    libsndfile1 portaudio19-dev \
    gfortran libopenblas-dev pkg-config \
    rt-tests
  info "  System packages OK"

  # --- 3) CPU governor = performance ---
  info "[3/6] Setting CPU governor to performance..."
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee "$g" >/dev/null
  done

  sudo tee /etc/systemd/system/cpu-performance.service >/dev/null <<'EOF'
[Unit]
Description=Set CPU governor to performance
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/bash -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g"; done'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
  sudo systemctl daemon-reload
  sudo systemctl enable --now cpu-performance.service >/dev/null 2>&1 || true
  info "  CPU governor = performance (persistent)"

  # --- 4) Create venv inside project dir (single canonical location) ---
  info "[4/6] Creating venv at $VENV_DIR..."
  cd "$PROJECT_DIR"
  python3 -m venv .venv
  source .venv/bin/activate

  # Confirm isolation
  RESOLVED=$(which python)
  if [[ "$RESOLVED" != "$VENV_DIR/bin/python" ]]; then
    error "  venv activation failed — python resolves to $RESOLVED, expected $VENV_DIR/bin/python"
  fi
  info "  venv active: $RESOLVED"

  # --- 5) Install Python deps ---
  info "[5/6] Installing Python deps..."
  pip install --upgrade pip wheel setuptools -q

  PYMM=$(python -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
  info "  Python $PYMM detected"

  if [ "$PYMM" = "3.11" ] && [ -f edge/requirements_edge.txt ]; then
    info "  Using pinned training stack (edge/requirements_edge.txt)"
    pip install -r edge/requirements_edge.txt -q
    pip install onnx sounddevice soundfile librosa -q
  else
    info "  Using latest wheel-backed stack (Python >= 3.12 path)"
    pip install --index-url https://download.pytorch.org/whl/cpu torch -q
    pip install numpy scipy onnx onnxruntime \
                sounddevice soundfile librosa \
                asteroid-filterbanks -q
  fi

  # Verify imports
  python - <<'PY'
import importlib, sys
required = ('numpy','scipy','onnx','onnxruntime','sounddevice','soundfile','torch')
missing = []
for m in required:
    try:
        mod = importlib.import_module(m)
        print(f"  OK  {m} {getattr(mod, '__version__', '')}")
    except Exception as e:
        missing.append(f"{m}: {e}")
if missing:
    print("FAIL — missing:")
    for line in missing: print(f"  - {line}")
    sys.exit(1)
PY
  info "  All deps OK"

  # --- 6) RT scheduling capabilities (no sudo needed at runtime) ---
  info "[6/6] Granting RT scheduling capabilities..."

  # Cap on venv Python binary
  REAL_PY=$(readlink -f .venv/bin/python3)
  sudo setcap 'cap_sys_nice=eip' "$REAL_PY"
  info "  cap_sys_nice granted to $REAL_PY"

  # Cap on chrt binary
  CHRT_BIN=$(which chrt)
  sudo setcap 'cap_sys_nice=eip' "$CHRT_BIN"
  info "  cap_sys_nice granted to $CHRT_BIN"

  # Verify
  getcap "$REAL_PY"
  getcap "$CHRT_BIN"

  # Disable RT throttle (prevents 50ms periodic spike)
  echo -1 | sudo tee /proc/sys/kernel/sched_rt_runtime_us >/dev/null
  sudo tee /etc/sysctl.d/99-rt-audio.conf >/dev/null <<'EOF'
kernel.sched_rt_runtime_us = -1
EOF
  sudo sysctl -p /etc/sysctl.d/99-rt-audio.conf >/dev/null
  info "  RT throttle disabled (sched_rt_runtime_us = -1)"

  echo ""
  info "================================================================="
  info "  Bootstrap complete. Run again with --run-only to skip setup."
  info "================================================================="
  echo ""

fi  # end setup block

# =============================================================================
# PIPELINE RUN
# =============================================================================
# The pipeline already supports --rt-fifo-priority internally via
# try_set_realtime_fifo(), so we do not wrap it in external `chrt` here.


cd "$PROJECT_DIR"
source .venv/bin/activate

# Confirm venv is correct
RESOLVED=$(which python)
if [[ "$RESOLVED" != "$VENV_DIR/bin/python" ]]; then
  error "venv not active — python resolves to $RESOLVED"
fi

# Preflight: imports
python - <<'PY'
import importlib, sys
required = ('numpy','scipy','onnxruntime','sounddevice','torch')
missing = [m for m in required if not __import__('importlib').util.find_spec(m)]
if missing:
    print(f"PREFLIGHT FAIL: {missing}")
    print("Run:  bash bootstrap.sh   (without --run-only) to reinstall")
    sys.exit(1)
print("[preflight] all deps OK")
PY

# Preflight: cyclictest latency check (quick 1s sample)
info "Checking scheduler latency (1s sample)..."
MAX_LAT=$(sudo cyclictest -l5000 -m -n -a0 -t1 -p99 -i200 -q 2>/dev/null | grep "Max Latencies" | awk '{print $NF}' || echo "unknown")
if [[ "$MAX_LAT" != "unknown" ]]; then
  info "  cyclictest max latency: ${MAX_LAT} µs"
  if (( MAX_LAT > 100000 )); then
    warn "  Max latency > 100ms — consider installing PREEMPT_RT kernel (see §3a)"
  fi
fi

# Build model selection flags
if [ "$USE_ZOO" = true ]; then
  MODEL_FLAGS="--zoo-dir $ZOO_DIR --bubble-radius $BUBBLE_RADIUS"
else
  MODEL_FLAGS="--model $MODEL_PATH --contract-path $CONTRACT_PATH"
fi

echo ""
info "Starting pipeline..."
info "  model:   $([ "$USE_ZOO" = true ] && echo "zoo=$ZOO_DIR radius=${BUBBLE_RADIUS}m" || echo "$MODEL_PATH")"
info "  devices: in=$INPUT_DEVICE out=$OUTPUT_DEVICE"
info "  rt_fifo: priority=$RT_FIFO_PRIORITY"
echo ""

.venv/bin/python edge/inference_pipeline.py \
  $MODEL_FLAGS \
  --input-device "$INPUT_DEVICE" \
  --output-device "$OUTPUT_DEVICE" \
  --input-channels "$INPUT_CHANNELS" \
  --output-channels "$OUTPUT_CHANNELS" \
  --channel-map "$CHANNEL_MAP" \
  --input-sr "$INPUT_SR" \
  --model-sr "$MODEL_SR" \
  --output-sr "$OUTPUT_SR" \
  --intra-op-threads "$INTRA_OP_THREADS" \
  --inter-op-threads "$INTER_OP_THREADS" \
  --hops-per-io "$HOPS_PER_IO" \
  --rt-fifo-priority "$RT_FIFO_PRIORITY" \
  --mix-dry "$MIX_DRY" \
  --silence-dbfs "$SILENCE_DBFS" \
  --max-steps-per-block "$MAX_STEPS_PER_BLOCK" \
  --profile
