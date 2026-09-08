param()

# ---------- CONFIG ----------
$PiUser   = "soundbubble"
$PiHost   = "soundpi.local"     # or Pi IP
$PiRepo   = "/home/$PiUser/Sound_Bubble"

$LocalOnnx      = "C:\Users\darre\Sound_Bubble\edge\model.onnx"
$LocalContract  = "C:\Users\darre\Sound_Bubble\edge\model.runtime.json"

# Model zoo (optional). If $UseZoo = $true, syncs the whole zoo dir and selects
# by --bubble-radius at runtime. $LocalZooDir should contain paired
# <name>.onnx + <name>.runtime.json files produced by edge/export_to_onnx.py.
$UseZoo        = $false
$LocalZooDir   = "C:\Users\darre\Sound_Bubble\edge\zoo"
$BubbleRadius  = 1.5

# Device IDs on Pi (from `python -c "import sounddevice as sd; print(sd.query_devices())"`).
# Pick the real hw:*,* devices, NOT the 'sysdefault' / 'default' / 'dmix' virtual aliases —
# those route through dmix/plug and will xrun the moment the model produces real signal.
# Example mapping on this rig:
#   [0] Sound Blaster X1 (hw:0,0) out=8 default_sr=48000  -> OUTPUT
#   [1] INMP441 8ch      (hw:3,0) in=8  default_sr=44100  -> INPUT
$InputDeviceIdx  = 1
$OutputDeviceIdx = 0

# Audio config
$InputChannels = 8
$OutputChannels = 2
$ChannelMap = "0,1,2,3,4,5"
$InputSR = 44100
$ModelSR = 24000
$OutputSR = 48000

# ONNX Runtime threading. Pi 5 has 4 cores; keep intra <= cores-1 so the
# PortAudio output callback thread can schedule and hold its GIL slot,
# otherwise you will hit the ALSA xrun cascade the moment real signal
# arrives. See PiSetup_BringUp.md §7a.
$IntraOpThreads = 3
$InterOpThreads = 1
# Output ring-buffer depth in model hops. 4 = lowest latency, 8 = jitter-tolerant.
$HopsPerIo = 8

# SCHED_FIFO wrapper. Required on this rig for crackle-free --mix-dry 1.0.
# 'sudo' -> password prompt once over ssh -t (works because we allocate a TTY).
# If you've run `sudo setcap cap_sys_nice+eip "$(readlink -f .venv/bin/python3)"`
# on the Pi, set $UseSudoForChrt = $false and it'll run without prompting.
# Set $UseRtPrio = $false to disable entirely (not recommended).
$UseRtPrio      = $true
$UseSudoForChrt = $true

# Determinism gates (recommended)
$CheckCpuGovernor      = $true
$RequireCpuPerformance = $true
$CheckPreemptRt        = $true
$RequirePreemptRt      = $false

# ---------- RUN ----------
$Timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$RemoteLog = "$PiRepo/logs/live_pipeline_$Timestamp.log"

Write-Host "[1/5] Check SSH"
ssh "$PiUser@$PiHost" "echo connected"

Write-Host "[1b/5] Preflight: kernel + governor"
if ($CheckPreemptRt) {
  # Prefer kernel-config check: Pi OS builds don't consistently surface PREEMPT_RT in uname strings.
  $Cfg = ssh "$PiUser@$PiHost" "grep -i PREEMPT_RT /boot/config-`$(uname -r) 2>/dev/null || true"
  if ($Cfg -notmatch "PREEMPT_RT") {
    $Msg = "[warn] PREEMPT_RT not detected via /boot/config-$(uname -r). Determinism risk (scheduler spikes)."
    if ($RequirePreemptRt) {
      Write-Host $Msg -ForegroundColor Red
      Write-Host "Set `$RequirePreemptRt = `$false to continue anyway." -ForegroundColor Red
      exit 2
    } else {
      Write-Host $Msg -ForegroundColor Yellow
    }
  } else {
    Write-Host "[ok] PREEMPT_RT detected (kernel config)" -ForegroundColor Green
  }
}

if ($CheckCpuGovernor) {
  $Gov = ssh "$PiUser@$PiHost" "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown"
  if ($Gov -ne "performance") {
    $Msg = "[warn] CPU governor is '$Gov' (expected 'performance')."
    if ($RequireCpuPerformance) {
      Write-Host $Msg -ForegroundColor Red
      Write-Host "Fix on Pi: enable cpu-performance.service (see bring-up notes), then retry." -ForegroundColor Red
      exit 3
    } else {
      Write-Host $Msg -ForegroundColor Yellow
    }
  } else {
    Write-Host "[ok] CPU governor = performance" -ForegroundColor Green
  }
}

Write-Host "[2/5] Ensure logs dir + upload model artifacts"
ssh "$PiUser@$PiHost" "mkdir -p $PiRepo/logs"
if ($UseZoo) {
  if (-not (Test-Path $LocalZooDir)) {
    Write-Host "[zoo] LocalZooDir '$LocalZooDir' not found. Aborting." -ForegroundColor Red
    exit 1
  }
  Write-Host "[zoo] syncing $LocalZooDir -> Pi:$PiRepo/edge/zoo/"
  ssh "$PiUser@$PiHost" "mkdir -p $PiRepo/edge/zoo"
  scp -r "$LocalZooDir\*" "$PiUser@$PiHost`:$PiRepo/edge/zoo/"
} else {
  scp "$LocalOnnx" "$PiUser@$PiHost`:$PiRepo/edge/model.onnx"
  scp "$LocalContract" "$PiUser@$PiHost`:$PiRepo/edge/model.runtime.json"
}

Write-Host "[2b/5] Preflight: verify Pi venv has required Python deps"
$Preflight = @"
set -e
cd $PiRepo
source .venv/bin/activate
python - <<'PY'
import importlib, sys
required = ('numpy','scipy','onnx','onnxruntime','sounddevice','soundfile','torch')
missing = []
for m in required:
    try: importlib.import_module(m)
    except Exception as e: missing.append(f'{m}: {e}')
if missing:
    print('PREFLIGHT FAIL:')
    for line in missing: print('  -', line)
    print('Fix: source .venv/bin/activate && pip install -r edge/requirements_edge.txt && pip install onnx sounddevice soundfile librosa')
    sys.exit(1)
print('preflight ok')
PY
"@
ssh "$PiUser@$PiHost" "bash -lc '$Preflight'"
if ($LASTEXITCODE -ne 0) {
  Write-Host "Preflight failed. Run the bootstrap script on Pi and retry." -ForegroundColor Red
  exit 1
}

Write-Host "[3/5] Start live pipeline (CTRL+C to stop)"
if     ($UseRtPrio -and $UseSudoForChrt) { $RtPrefix = "sudo chrt -f 50 " }
elseif ($UseRtPrio)                      { $RtPrefix = "chrt -f 50 "      }
else                                     { $RtPrefix = ""                 }
$PyBin = if ($UseRtPrio) { ".venv/bin/python" } else { "python" }
if ($UseZoo) {
  $ModelSelect = "--zoo-dir edge/zoo --bubble-radius $BubbleRadius"
} else {
  $ModelSelect = "--model edge/model.onnx --contract-path edge/model.runtime.json"
}
$Cmd = @"
set -e
cd $PiRepo
source .venv/bin/activate
echo '[info] starting live pipeline' | tee -a $RemoteLog
$RtPrefix$PyBin edge/inference_pipeline.py \
  $ModelSelect \
  --input-device $InputDeviceIdx \
  --output-device $OutputDeviceIdx \
  --input-channels $InputChannels \
  --output-channels $OutputChannels \
  --channel-map $ChannelMap \
  --input-sr $InputSR \
  --model-sr $ModelSR \
  --output-sr $OutputSR \
  --intra-op-threads $IntraOpThreads \
  --inter-op-threads $InterOpThreads \
  --hops-per-io $HopsPerIo \
  --profile 2>&1 | tee -a $RemoteLog
"@

ssh -t "$PiUser@$PiHost" "bash -lc '$Cmd'"

Write-Host "[4/5] Pipeline stopped"
Write-Host "[5/5] Log saved on Pi: $RemoteLog"
Write-Host "To view: ssh $PiUser@$PiHost `"tail -n 200 $RemoteLog`""