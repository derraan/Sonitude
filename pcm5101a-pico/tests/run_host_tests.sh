#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
gcc -std=c11 -Wall -Wextra -Werror -O2 -I"$here/../src/include" "$here/test_host.c" -o "$out/test_host"
"$out/test_host"
