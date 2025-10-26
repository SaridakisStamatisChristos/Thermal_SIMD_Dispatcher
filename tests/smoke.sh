#!/usr/bin/env bash
set -euo pipefail
# Try to relax perf paranoia if sudo is available
if command -v sudo >/dev/null 2>&1; then
  sudo sysctl -w kernel.perf_event_paranoid=0 || true
fi
# Short demo run
./thermal_simd --duration-sec=1 --work-iters=2000000 || true
echo "OK: smoke"
