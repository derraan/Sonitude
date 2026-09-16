# Pi field bring-up runbook (Sound Bubble + Sonitude)

Field notes for the Raspberry Pi 5 headset rig. This is the document referenced by
`edge/rpi5/run_pi_live_pipeline.ps1` (`PiSetup_BringUp.md`).

Two pipelines share the same mic array and Pi host. Keep them straight:

| Pipeline | Tree / binary | Purpose |
| --- | --- | --- |
| **Sound Bubble (neural)** | `Sound_Bubble/` → `edge/inference_pipeline.py` | ONNX sound-bubble inference |
| **Sonitude (C++ RT)** | repo root → `build/sonitude_realtime` | Near-field MVDR + suppression + binaural |

There is **no CI → Pi deploy**. Merged GitHub PRs reach the Pi only when you
**pull (or sync) the tree and rebuild / reinstall deps** on the device.

Assumed layout on the Pi (adjust user/path if yours differs):

```text
/home/soundbubble/Sonitude/          # full git clone of this monorepo
  ├── config/                        # Sonitude YAML (default.yaml, geometry, calibration)
  ├── build/                         # CMake build dir for sonitude_realtime
  └── Sound_Bubble/                  # neural edge tree (venv lives here)
        ├── .venv/
        └── edge/
```

If you keep Sound Bubble in a separate clone (`/home/soundbubble/Sound_Bubble`),
use that path for §1–§4 and §7; use the Sonitude clone path for §5–§6.

---

## 1) First-time Sound Bubble bootstrap

On the Pi, inside the Sound Bubble tree:

```bash
cd /home/soundbubble/Sonitude/Sound_Bubble   # or your Sound_Bubble clone
bash bootstrap_soundbubble.sh
```

What it does: cleans stale venvs, installs apt + Python deps into `.venv`,
sets CPU governor to `performance` (persistent `cpu-performance.service`),
grants `cap_sys_nice` for RT scheduling, and disables RT throttle.

Re-run the pipeline without repeating setup:

```bash
bash bootstrap_soundbubble.sh --run-only
```

Also see [`../README_EDGE.md`](../README_EDGE.md) for export / benchmark / live CLI details.

---

## 2) Pick real ALSA / PortAudio devices

Always use the real `hw:*,*` endpoints — never `sysdefault` / `default` / `dmix`.
Those aliases insert a mixer/resampler and xrun as soon as the model produces signal.

```bash
cd /home/soundbubble/Sonitude/Sound_Bubble
source .venv/bin/activate
python -c "import sounddevice as sd; print(sd.query_devices())"
arecord -l
aplay -l
```

Record the indices for Pico (or INMP441) capture and the USB DAC / headphone out.
Put them in:

- `bootstrap_soundbubble.sh` (`INPUT_DEVICE` / `OUTPUT_DEVICE`)
- `edge/rpi5/run_pi_live_pipeline.ps1` (`$InputDeviceIdx` / `$OutputDeviceIdx`)
- Sonitude `config/default.pi.yaml` (`capture.alsa_device` / `playback.alsa_device`)

---

## 3) Deploy / refresh ONNX models (laptop → Pi)

From Windows, edit paths in `edge/rpi5/run_pi_live_pipeline.ps1`, then:

```powershell
pwsh edge/rpi5/run_pi_live_pipeline.ps1
```

That script: SSH preflight → scp `model.onnx` + `model.runtime.json` (or the zoo) →
start `inference_pipeline.py` under `chrt`. It does **not** sync git commits.

Optional whole-tree archive (manual USB/scp): `edge/rpi5/archiveRepo.ps1`.

---

## 4) Updating Sound Bubble code after merges

When `Sound_Bubble/` commits land on `main`:

```bash
cd /home/soundbubble/Sonitude          # monorepo
git fetch origin
git checkout main
git pull --ff-only origin main

cd Sound_Bubble
source .venv/bin/activate
# If requirements changed:
pip install -r edge/requirements_edge.txt
# Re-grant RT caps if the venv python binary was replaced:
sudo setcap 'cap_sys_nice=eip' "$(readlink -f .venv/bin/python3)"

bash bootstrap_soundbubble.sh --run-only
```

If only the ONNX weights changed, scp the new artifacts (§3) — no full bootstrap needed.

---

## 5) Updating Sonitude C++ RT after merges (MVDR / geometry / YAML)

This is the path for the adaptive near-field MVDR stack (e.g. PRs that change
`src/dsp/beamformer*`, `config/default.yaml` `spatial.mvdr`, or head-frame geometry).

On the Pi:

```bash
cd /home/soundbubble/Sonitude
git fetch origin
git checkout main
git pull --ff-only origin main

# One-time deps on Raspberry Pi OS:
sudo apt update
sudo apt install -y cmake ninja-build g++ pkg-config \
  libasound2-dev libyaml-cpp-dev libspdlog-dev libsamplerate0-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONITUDE_FETCH_DEPS=OFF
cmake --build build --parallel

# Keep device-specific ALSA names out of tracked default.yaml:
#   cp config/default.yaml config/default.pi.yaml
#   edit capture/playback alsa_device, then:
./build/sonitude_realtime --config config/default.pi.yaml --validate-config
./build/sonitude_device_probe --config config/default.pi.yaml

# Beamform (not passthrough):
./build/sonitude_realtime --config config/default.pi.yaml --mode beamform
```

### Config that must stay consistent after this stack

| File | Requirement |
| --- | --- |
| `config/default.yaml` / `default.pi.yaml` | `spatial.backend: adaptive_geometric` needs a full `spatial.mvdr` block (`diag_load`, `max_white_noise_gain`, `cov_tau_sec`) |
| `config/geometry_soundbubble_initial.yaml` | `frame: head_frame_v1` with `right=+X`, `forward=+Y`, `up=+Z`; USB0 on −X, USB5 on +X |
| `config/calibration_*.yaml` | Channel IDs must match geometry mic IDs |
| HRTF path | `steering.kemar_lut.table_path` / binaural table must exist on the Pi |

`--validate-config` fails closed if `spatial.mvdr` or the geometry frame is missing/wrong — fix YAML before chasing audio bugs.

`scripts/run_realtime.sh` launches **passthrough only**. For the new beamformer always use `--mode beamform`.

---

## 6) Quick field checklist (before a demo)

1. `git pull --ff-only origin main` in the Sonitude clone; rebuild C++ if `src/` or `config/` schema changed.
2. CPU governor is `performance` (`cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`).
3. Capture/playback are `hw:` devices; Pico enumerates; DAC plays.
4. Sonitude: `--validate-config` then `--mode beamform` with `default.pi.yaml`.
5. Sound Bubble: venv active, model + `*.runtime.json` present, `bootstrap_soundbubble.sh --run-only` (or the PS1 launcher).
6. Confirm you are running the pipeline you intend (neural vs MVDR) — they are not interchangeable.

---

## 7) Realtime / threading notes

### 7a) ONNX Runtime threads vs PortAudio (xrun cascade)

Pi 5 has 4 cores. Keep ORT **intra-op threads ≤ 3** so the PortAudio output callback
can schedule and hold its GIL slot. If intra-op saturates all cores, ALSA xruns
appear the moment real signal (not silence) is produced.

Defaults used in the field scripts:

- `INTRA_OP_THREADS=3` / `$IntraOpThreads = 3`
- `INTER_OP_THREADS=1`
- `HOPS_PER_IO=8` (4 = lowest latency, 8 = more jitter tolerance)
- `SCHED_FIFO` via `chrt -f 50` (or `cap_sys_nice` on the venv python)

### 7b) Optional PREEMPT_RT kernel

Not required by bootstrap. If you install an RT kernel, verify with:

```bash
grep -i PREEMPT_RT /boot/config-$(uname -r)
```

See also [by/RT-Kernel](https://github.com/by/RT-Kernel) (linked from `bootstrap_soundbubble.sh`).

---

## 8) Remote Tailscale controller (optional)

HTTP control of `inference_pipeline.py` over Tailscale:

- Service unit: `edge/rpi5/soundbubble-controller.service`
- Token env: `/etc/soundbubble-controller.env`
- Docs: [`../README_EDGE.md`](../README_EDGE.md) (Remote controller section),
  [`../REMOTE_CONTROL_TAILSCALE_PLAN.md`](../REMOTE_CONTROL_TAILSCALE_PLAN.md)

Bind to `0.0.0.0` only behind Tailscale/firewall + bearer token.

---

## 9) Related paths

| Doc / script | Role |
| --- | --- |
| `Sound_Bubble/bootstrap_soundbubble.sh` | First-time Pi bootstrap + `--run-only` |
| `Sound_Bubble/edge/README_EDGE.md` | Export, benchmark, live CLI |
| `Sound_Bubble/edge/EDGE_RUNTIME.md` | Capture queue / overload / reset policy |
| `Sound_Bubble/edge/rpi5/run_pi_live_pipeline.ps1` | Laptop SSH/scp live launch |
| Repo root `README.md` | Sonitude CMake build |
| `docs/device_setup.md` | Sonitude ALSA / RT environment goals |
| `config/default.yaml` | Sonitude runtime (prefer a Pi-local `default.pi.yaml` for ALSA names) |
