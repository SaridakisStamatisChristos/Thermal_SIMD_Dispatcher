#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build-ci" -DCMAKE_BUILD_TYPE=Release
cmake --build "${REPO_ROOT}/build-ci" --target thermal_simd test_thermal_simd -j"$(nproc)"
"${REPO_ROOT}/build-ci/test_thermal_simd"
"${REPO_ROOT}/build-ci/thermal_simd" --health-check
