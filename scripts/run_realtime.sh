#!/usr/bin/env bash
set -euo pipefail

CONFIG_PATH="${1:-config/production_pi.yaml}"

echo "[Sonitude] launching production passthrough runtime with ${CONFIG_PATH}"
./build/sonitude_realtime --config "${CONFIG_PATH}" --mode passthrough
