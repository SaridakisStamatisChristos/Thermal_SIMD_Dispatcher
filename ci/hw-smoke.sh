#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

if ! lscpu | grep -Eq '(^|[[:space:]])avx512f([[:space:]]|$)'; then
    echo "HIL runner is labelled avx512 but AVX-512F is not exposed by the host" >&2
    exit 1
fi

cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build-ci" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "${REPO_ROOT}/build-ci" --target thermal_simd test_thermal_simd -j"$(nproc)"
"${REPO_ROOT}/build-ci/test_thermal_simd"
# The HIL lane is specifically AVX-512-labelled, so opt in explicitly rather
# than relying on the runtime's conservative default (AVX-512 disabled).
"${REPO_ROOT}/build-ci/thermal_simd" --allow-avx512 --health-check
