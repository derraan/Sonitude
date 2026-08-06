#!/usr/bin/env bash
set -euo pipefail

echo "[Milestone 0 scaffold] ALSA probing script placeholder."
echo "Milestone 1 will implement card/device probing and hw-params dumps."
echo
echo "Useful commands:"
echo "  aplay -l"
echo "  arecord -l"
echo "  arecord --dump-hw-params -D hw:<card>,<device> -f S16_LE -c 6 -r 44100 -d 1 /tmp/null.wav"
echo "  aplay --dump-hw-params -D hw:<card>,<device> /usr/share/sounds/alsa/Front_Center.wav"
