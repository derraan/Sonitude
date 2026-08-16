# Linux/Pi Hardware Gate Runbook (PR #28)

This runbook captures remaining Raspberry Pi gates after the 2026-08-15 short
passthrough evidence run at `cb8b522`.

## Scope and baseline profile

- Use `config/production_pi.yaml` for hardware gates.
- Baseline runtime state for this profile:
  - `odas.enabled: false`
  - `suppression.enabled: false`
  - `require_realtime: true`
  - `require_memory_lock: true`
- The 2026-08-15 tested local IDs and map were:
  - capture `hw:active,0`
  - playback `hw:X1,0`
  - `active_channel_map: [5,4,3,2,1,0]`
- Always rediscover IDs locally before each run:

```bash
arecord -l
aplay -l
```

Do not substitute numeric card positions (`hw:0,0`) in tracked production
config.

## 0) Linux target setup and baseline build

```bash
sudo apt update
sudo apt install -y cmake ninja-build g++ libasound2-dev libyaml-cpp-dev libspdlog-dev libsamplerate0-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSONITUDE_FETCH_DEPS=OFF \
  -DSONITUDE_WITH_ALSA=ON \
  -DSONITUDE_WITH_LIBSAMPLERATE=ON \
  -DSONITUDE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/sonitude_realtime --config config/production_pi.yaml --validate-config
```

Expected: build/test/config validation pass before hardware gates.

## 1) One-hour passthrough soak (three-thread scheduling)

Start with a unique transient unit and capture `MainPID`:

```bash
mkdir -p build/logs
UNIT="sonitude-pt-$(date +%Y%m%d-%H%M%S)"
sudo systemd-run --unit "$UNIT" --same-dir --collect \
  ./build/sonitude_realtime --config config/production_pi.yaml --mode passthrough
sleep 2
MAINPID="$(systemctl show "$UNIT" -p MainPID --value)"
echo "unit=$UNIT mainpid=$MAINPID" | tee "build/logs/${UNIT}.meta"
```

Capture live scheduling while process is running:

```bash
ps -L -p "$MAINPID" -o pid,tid,cls,rtprio,pri,comm | tee "build/logs/${UNIT}.sched.running.txt"
```

Allow one-hour runtime, then stop and collect PID-filtered journal:

```bash
sleep 3600
sudo systemctl stop "$UNIT"
journalctl -u "$UNIT" --no-pager | rg "pid=${MAINPID}|thread scheduling|telemetry:|final_counters:" > "build/logs/${UNIT}.journal.pid.txt"
```

Extract first/final telemetry plus final summary:

```bash
rg -n "telemetry:" "build/logs/${UNIT}.journal.pid.txt" > "build/logs/${UNIT}.telemetry.lines.txt"
python3 - <<'PY'
from pathlib import Path
p = Path("build/logs")
for telem in sorted(p.glob("sonitude-pt-*.telemetry.lines.txt")):
    lines = telem.read_text(encoding="utf-8").splitlines()
    out = telem.with_suffix(".summary.txt")
    if not lines:
        out.write_text("no telemetry lines found\n", encoding="utf-8")
        continue
    out.write_text(
        "\n".join([
            f"count={len(lines)}",
            f"first={lines[0]}",
            f"last={lines[-1]}",
        ]) + "\n",
        encoding="utf-8",
    )
PY
```

Hard-failure counter scan (nonzero means fail):

```bash
rg -n "cap_xruns=[1-9]|pb_xruns=[1-9]|pb_write_fail=[1-9]|cap_wait_timeouts=[1-9]|cap_wait_errors=[1-9]|cap_overflow_refusals=[1-9]|block_commit_fail=[1-9]|pool_exhausted=[1-9]|cap_deadline_misses=[1-9]|limiter_output_saturation=[1-9]" \
  "build/logs/${UNIT}.journal.pid.txt"
```

Notes:
- `playback_empty_waits` is an empty software-ready-queue metric and is not by
  itself an ALSA underrun failure.
- Use ALSA XRUN counters (`cap_xruns`, `pb_xruns`) as authoritative device
  underrun indicators.

## 2) One-hour beamform soak (four-thread scheduling)

Run the same procedure with `--mode beamform` and a distinct unit prefix:

```bash
UNIT="sonitude-bf-$(date +%Y%m%d-%H%M%S)"
sudo systemd-run --unit "$UNIT" --same-dir --collect \
  ./build/sonitude_realtime --config config/production_pi.yaml --mode beamform
```

Expected scheduling rows:
- capture/DSP `SCHED_FIFO/80`
- playback `SCHED_FIFO/78`
- telemetry `SCHED_OTHER/0`
- control `SCHED_OTHER/0`

## 3) Corrected static beam and scripted/live ODAS gates

These remain explicit acceptance gates and are not closed by passthrough soak.

- Corrected static beam direction checks with the corrected geometry profile.
- Scripted mock steering transition checks in beamform mode.
- Live ODAS disconnect/recovery checks only after scripted beamform passes.

Record each gate with command history and bounded summaries; do not paste full
journals into repository docs.

## 4) Calibration hardware capture gate

Use production profile and approved threshold:

```bash
MIN_CORRELATION="<PROJECT_APPROVED_MIN_CORRELATION>"
./build/sonitude_calibration_capture --config config/production_pi.yaml --seconds 10 --output build/calibration_capture_pi.wav
./build/sonitude_calibration_estimate \
  --input build/calibration_capture_pi.wav \
  --output build/calibration_estimate_pi.yaml \
  --config config/production_pi.yaml \
  --min-correlation "$MIN_CORRELATION"
```

`--synthetic` artifacts are CI-only and non-hardware evidence.

## 5) Fault-injection and shutdown gates

Pending explicit gates:
- forced playback failure with bounded shutdown
- blocked-capture SIGINT teardown
- xrun recovery under contention
- repeated process/service start-stop
- latency loopback (median/p95/p99)

## 6) Artifact hashing and provenance

Hash runtime artifacts without hashing `SHA256SUMS` into itself:

```bash
mkdir -p build/evidence
cp ./build/sonitude_realtime build/evidence/
cp config/production_pi.yaml config/geometry_soundbubble_xyz_v1.yaml config/calibration_example.yaml build/evidence/
(
  cd build/evidence
  sha256sum sonitude_realtime production_pi.yaml geometry_soundbubble_xyz_v1.yaml calibration_example.yaml > SHA256SUMS
)
```

If firmware artifact is available for the same run, include its hash in the
evidence bundle.

## Exit criteria template

Mark a gate complete only when evidence includes:
- exact commands used
- unit name and `MainPID`
- PID-filtered journal extract path
- first/final telemetry and final summary line
- pass/fail decision and follow-up issue for failures
