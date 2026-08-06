#!/usr/bin/env bash
set -euo pipefail

CONFIG_PATH="${1:-config/default.yaml}"

echo "[Sonitude M2] launching passthrough runtime"
chrt -f 85 ./build/sonitude_realtime --config "${CONFIG_PATH}" --mode passthrough
