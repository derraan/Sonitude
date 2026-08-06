#!/usr/bin/env bash
set -euo pipefail

echo "[Sonitude M1] Enumerating ALSA playback/capture devices"
aplay -l || true
arecord -l || true
echo
echo "[Sonitude M1] Probe capture hw params"
arecord --dump-hw-params -D "${1:-hw:PicoMic,0}" -f S16_LE -c 8 -r 44100 -d 1 /tmp/sonitude_probe_capture.wav || true
echo
echo "[Sonitude M1] Probe playback hw params"
aplay --dump-hw-params -D "${2:-hw:Creative,0}" /usr/share/sounds/alsa/Front_Center.wav || true
echo
echo "[Sonitude M1] USB IDs (Pico mic expected cafe:401a)"
lsusb | rg -i "cafe|creative" || true
