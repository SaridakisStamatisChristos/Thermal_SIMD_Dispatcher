#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${REPO_ROOT}/build-ci"}
SOAK_MINUTES=${SOAK_MINUTES:-120}
PATCH_THREADS=${PATCH_THREADS:-4}
PATCH_ITERATIONS=${PATCH_ITERATIONS:-2000}
SIGNAL_DURATION=${SIGNAL_DURATION:-30}
SIGNAL_RATE=${SIGNAL_RATE:-300}
TELEMETRY_CYCLES=${TELEMETRY_CYCLES:-3}

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "${BUILD_DIR}" --target stress_patch_request stress_signal_storm stress_telemetry_faults -j"$(nproc)"

END_TIME=$((SECONDS + SOAK_MINUTES * 60))
ROUND=0
while [ ${SECONDS} -lt ${END_TIME} ]; do
    ROUND=$((ROUND + 1))
    echo "[thermal-soak] round=${ROUND} remaining=$((END_TIME - SECONDS))s"
    "${BUILD_DIR}/stress_patch_request" --threads="${PATCH_THREADS}" --iterations="${PATCH_ITERATIONS}"
    "${BUILD_DIR}/stress_signal_storm" --duration-seconds="${SIGNAL_DURATION}" --signal-rate="${SIGNAL_RATE}"
    "${BUILD_DIR}/stress_telemetry_faults" --cycles="${TELEMETRY_CYCLES}"
    sleep 5
done

echo "[thermal-soak] completed ${ROUND} rounds over ${SOAK_MINUTES} minutes"
