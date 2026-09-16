# Device Setup (Raspberry Pi / Linux host)

Runbook for ALSA device selection, real-time environment, and keeping the Pi
current after merges. For the combined Sound Bubble + Sonitude field checklist
(including `git pull` + C++ rebuild), see
[`Sound_Bubble/edge/rpi5/PiSetup_BringUp.md`](../Sound_Bubble/edge/rpi5/PiSetup_BringUp.md).

## Goals

- Select explicit ALSA `hw:<card>,<device>` capture and playback endpoints.
- Verify negotiated hardware rate/format/channel/period capabilities before start.
- Run real-time workers with safe scheduling and CPU governor settings.
- Rebuild `sonitude_realtime` on-device after merged PRs that touch DSP or YAML schema.

## Required checks before first real-time run

1. Enumerate ALSA cards/devices and record stable identifiers (`arecord -l` / `aplay -l`).
2. Probe hardware-supported formats/rates/periods for:
   - Pico capture interface
   - Creative Super X-Fi (or chosen playback DAC)
3. Confirm channel activity/order and silence on expected padding slots (if any).
4. Verify no desktop mixer/resampler is inserted in the critical path (`hw:` only).

## Updating after merged PRs (Sonitude C++ RT)

There is no automated deploy. On the Pi clone:

```bash
git fetch origin && git checkout main && git pull --ff-only origin main
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSONITUDE_FETCH_DEPS=OFF
cmake --build build --parallel
./build/sonitude_realtime --config config/default.pi.yaml --validate-config
./build/sonitude_realtime --config config/default.pi.yaml --mode beamform
```

Keep Pi-specific ALSA device names in `config/default.pi.yaml` (local copy of
`default.yaml`) so pulls do not overwrite them. After the MVDR/YAML stack,
`spatial.mvdr` and `geometry.frame: head_frame_v1` are required — see the field
bring-up doc §5.

## Real-time environment requirements

- CPU governor set to `performance`.
- Appropriate permissions for `SCHED_FIFO` and memory locking where required.
- Process priority assignment and safety fallback if privilege is unavailable.

## Expected startup summary block (target format)

```text
Capture device, card/device ID, channel format, actual rate, period, buffer
Playback device, card/device ID, channel format, actual rate, period, buffer
Requested and negotiated latency estimates
ASRC enabled/disabled and current ratio
Geometry profile and calibration profile names
ODAS connected/mock/unavailable
Current mode and active direction
```

## Scripts

- `scripts/verify_alsa_devices.sh` - list and probe relevant ALSA endpoints.
- `scripts/set_realtime_environment.sh` - apply governor and RT prerequisites.
- `scripts/run_realtime.sh` - launches **passthrough** only; use
  `sonitude_realtime --mode beamform` for the MVDR path.
