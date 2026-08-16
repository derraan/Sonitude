#!/usr/bin/env bash
set -euo pipefail

config_path="${1:-config/production_pi.yaml}"
mode="${2:-passthrough}"

if [[ ! -f "${config_path}" ]]; then
  echo "[Sonitude] error: config file not found: ${config_path}" >&2
  exit 1
fi

case "${mode}" in
  passthrough|beamform) ;;
  *)
    echo "[Sonitude] error: unsupported mode '${mode}' (expected passthrough or beamform)" >&2
    exit 1
    ;;
esac

resolved_config="$(python3 -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "${config_path}")"
echo "[Sonitude] config=${resolved_config} mode=${mode}"
./build/sonitude_realtime --config "${resolved_config}" --mode "${mode}"
