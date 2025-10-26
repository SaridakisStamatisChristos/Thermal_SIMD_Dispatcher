#!/usr/bin/env bash
set -euo pipefail
# Try to relax perf paranoia if sudo is available
if command -v sudo >/dev/null 2>&1; then
  sudo sysctl -w kernel.perf_event_paranoid=0 || true
fi
# Short demo run
./thermal_simd --duration-sec=1 --work-iters=2000000 || true

# Large cooldown inputs should not wrap tick counters
output=""
if output=$(./thermal_simd --duration-sec=1 --work-iters=1 \
  --cooldown-down=3600000 --cooldown-up=3600000 --min-dwell=3600000 2>&1); then
  ticks_line=$(printf '%s\n' "$output" | grep "Cooldown ticks:" || true)
  if [[ -z "$ticks_line" ]]; then
    printf '%s\n' "$output"
    echo "ERROR: missing cooldown tick output" >&2
    exit 1
  fi
  down_ticks=$(printf '%s\n' "$ticks_line" | sed -n 's/.*down=\([0-9]\+\).*/\1/p')
  up_ticks=$(printf '%s\n' "$ticks_line" | sed -n 's/.*up=\([0-9]\+\).*/\1/p')
  dwell_ticks=$(printf '%s\n' "$ticks_line" | sed -n 's/.*min-dwell=\([0-9]\+\).*/\1/p')
  if [[ -z "$down_ticks" || -z "$up_ticks" || -z "$dwell_ticks" ]]; then
    printf '%s\n' "$output"
    echo "ERROR: failed to parse cooldown tick output" >&2
    exit 1
  fi
  if (( down_ticks <= 0 || up_ticks <= 0 || dwell_ticks <= 0 )); then
    printf '%s\n' "$output"
    echo "ERROR: cooldown tick counts must be positive" >&2
    exit 1
  fi
else
  status=$?
  printf '%s\n' "$output"
  if [[ $status -eq 132 ]]; then
    echo "Skipping cooldown tick validation due to unsupported instruction set" >&2
  else
    echo "ERROR: large cooldown run failed" >&2
    exit 1
  fi
fi
echo "OK: smoke"
