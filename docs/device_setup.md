# Device Setup (Planned for Milestone 1+)

This runbook captures required Linux and ALSA setup tasks. Milestone 0 does not yet include real device I/O.

## Goals

- Select explicit ALSA `hw:<card>,<device>` capture and playback endpoints.
- Verify negotiated hardware rate/format/channel/period capabilities before start.
- Run real-time workers with safe scheduling and CPU governor settings.

## Required checks before first real-time run

1. Enumerate ALSA cards/devices and record stable identifiers.
2. Probe hardware-supported formats/rates/periods for:
   - Pico capture interface
   - Creative Super X-Fi (or chosen playback DAC)
3. Confirm channel activity/order and silence on expected padding slots (if any).
4. Verify no desktop mixer/resampler is inserted in the critical path.

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
- `scripts/run_realtime.sh` - launch executable with explicit config and priority.

These scripts are scaffold placeholders in Milestone 0 and become operational in later milestones.
