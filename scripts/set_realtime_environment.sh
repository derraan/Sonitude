#!/usr/bin/env bash
set -euo pipefail

echo "[Sonitude M2] Applying RT environment helpers"
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
  if [ -w "${cpu}/cpufreq/scaling_governor" ]; then
    echo performance | sudo tee "${cpu}/cpufreq/scaling_governor" >/dev/null
  fi
done
echo "ulimit -r: $(ulimit -r)"
echo "ulimit -l: $(ulimit -l)"
echo "If rtprio/memlock are low, update /etc/security/limits.conf for your user."
