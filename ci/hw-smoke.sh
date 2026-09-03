#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
# shellcheck source=ci/hil-common.sh
source "${SCRIPT_DIR}/hil-common.sh"

TARGET_ISA=$(hil_target_isa)
RUNTIME_ISA_ARGUMENT=$(hil_runtime_isa_argument "${TARGET_ISA}")
"${SCRIPT_DIR}/hil-preflight.sh" --skip-port

cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build-ci" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "${REPO_ROOT}/build-ci" --target thermal_simd test_thermal_simd -j"$(nproc)"
"${REPO_ROOT}/build-ci/test_thermal_simd"
"${REPO_ROOT}/build-ci/thermal_simd" "${RUNTIME_ISA_ARGUMENT}" --health-check --metrics-port=0
