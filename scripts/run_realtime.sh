#!/usr/bin/env bash
set -euo pipefail

CONFIG_PATH="${1:-config/default.yaml}"

echo "[Milestone 0 scaffold] running config validation only."
./build/sonitude_realtime --config "${CONFIG_PATH}" --validate-config
echo "Realtime audio path is not implemented in Milestone 0."
