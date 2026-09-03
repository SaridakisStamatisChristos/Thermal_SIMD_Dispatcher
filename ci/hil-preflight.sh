#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/hil-common.sh
source "${SCRIPT_DIR}/hil-common.sh"

CHECK_PORT=1
if [ "${1:-}" = "--skip-port" ]; then
    CHECK_PORT=0
elif [ "$#" -ne 0 ]; then
    echo "usage: $0 [--skip-port]" >&2
    exit 2
fi

TARGET_ISA=$(hil_target_isa)
for command_name in bash cmake curl git grep lscpu make nproc python3 taskset "${CC:-cc}" "${CXX:-c++}"; do
    hil_require_command "${command_name}"
done
hil_require_target_isa "${TARGET_ISA}"

AVAILABLE_CPUS=$(nproc)
if ! [[ "${AVAILABLE_CPUS}" =~ ^[0-9]+$ ]] || [ "${AVAILABLE_CPUS}" -lt 2 ]; then
    echo "HIL requires at least two CPUs in the current affinity/cpuset; found ${AVAILABLE_CPUS}" >&2
    exit 1
fi

if [ "${CHECK_PORT}" -eq 1 ]; then
    METRICS_PORT=${METRICS_PORT:-19464}
    if ! [[ "${METRICS_PORT}" =~ ^[0-9]+$ ]] || [ "${METRICS_PORT}" -lt 1 ] || [ "${METRICS_PORT}" -gt 65535 ]; then
        echo "METRICS_PORT must be an integer in [1, 65535]" >&2
        exit 2
    fi
    python3 - "${METRICS_PORT}" <<'PY'
import socket
import sys

port = int(sys.argv[1])
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    try:
        sock.bind(("127.0.0.1", port))
    except OSError as exc:
        raise SystemExit(f"metrics port 127.0.0.1:{port} is unavailable: {exc}")
PY
fi

TEMPERATURE_FOUND=0
for path in /sys/class/hwmon/hwmon*/temp*_input /sys/class/thermal/thermal_zone*/temp; do
    if [ -r "${path}" ]; then
        TEMPERATURE_FOUND=1
        break
    fi
done
if [ "${TEMPERATURE_FOUND}" -ne 1 ]; then
    echo "HIL preflight warning: no readable-looking Linux temperature path was discovered" >&2
fi
POWERCAP_FOUND=0
for path in /sys/class/powercap/*/energy_uj; do
    if [ -r "${path}" ]; then
        POWERCAP_FOUND=1
        break
    fi
done
if [ "${POWERCAP_FOUND}" -ne 1 ]; then
    echo "HIL preflight note: RAPL/powercap energy is unavailable; power results will be omitted" >&2
fi

echo "HIL preflight passed: target=${TARGET_ISA} available_cpus=${AVAILABLE_CPUS} metrics_port=${METRICS_PORT:-skipped}"
