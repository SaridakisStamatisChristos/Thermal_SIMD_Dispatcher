#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
# shellcheck source=ci/hil-common.sh
source "${REPO_ROOT}/ci/hil-common.sh"

fail() {
    echo "test_hil_common: $*" >&2
    exit 1
}

assert_equal() {
    if [ "$1" != "$2" ]; then
        fail "expected '$2', got '$1'"
    fi
}

unset HIL_TARGET_ISA HIL_RUN_ID
assert_equal "$(hil_target_isa)" "avx2"
assert_equal "$(hil_required_cpu_flag avx2)" "avx2"
assert_equal "$(hil_required_cpu_flag avx512)" "avx512f"
assert_equal "$(hil_runtime_isa_argument avx2)" "--no-avx512"
assert_equal "$(hil_runtime_isa_argument avx512)" "--allow-avx512"
assert_equal "$(hil_expected_width avx2)" "avx2"
assert_equal "$(hil_expected_width avx512)" "avx512"

HIL_TARGET_ISA=invalid
if hil_target_isa >/dev/null 2>&1; then
    fail "invalid target ISA was accepted"
fi
unset HIL_TARGET_ISA

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/tsd-hil-test.XXXXXX")
cleanup() {
    if [ -n "${TEST_ROOT:-}" ] && [ -d "${TEST_ROOT}" ] &&
       [[ "$(basename -- "${TEST_ROOT}")" == tsd-hil-test.* ]]; then
        rm -rf -- "${TEST_ROOT}"
    fi
}
trap cleanup EXIT

ARTIFACT_ROOT="${TEST_ROOT}/artifacts"
HIL_RUN_ID=valid-run
RUN_DIR=$(hil_prepare_run_directory "${ARTIFACT_ROOT}" "${REPO_ROOT}" avx2)
assert_equal "${RUN_DIR}" "${ARTIFACT_ROOT}/valid-run"
[ -d "${RUN_DIR}" ] || fail "run directory was not created"
touch "${ARTIFACT_ROOT}/sentinel"
if hil_prepare_run_directory "${ARTIFACT_ROOT}" "${REPO_ROOT}" avx2 >/dev/null 2>&1; then
    fail "existing run directory was reused"
fi
[ -f "${ARTIFACT_ROOT}/sentinel" ] || fail "existing artifacts were modified"

HIL_RUN_ID='../escape'
if hil_prepare_run_directory "${TEST_ROOT}/escape-root" "${REPO_ROOT}" avx2 >/dev/null 2>&1; then
    fail "unsafe run identifier was accepted"
fi
[ ! -e "${TEST_ROOT}/escape" ] || fail "unsafe run identifier escaped the artifact root"

HIL_RUN_ID=unsafe-root
if hil_prepare_run_directory / "${REPO_ROOT}" avx2 >/dev/null 2>&1; then
    fail "filesystem root was accepted as an artifact root"
fi
if hil_prepare_run_directory "${REPO_ROOT}" "${REPO_ROOT}" avx2 >/dev/null 2>&1; then
    fail "repository root was accepted as an artifact root"
fi

ORIGINAL_HOME=${HOME:-}
HOME="${TEST_ROOT}/fake-home"
mkdir -p "${HOME}"
if hil_prepare_run_directory "${HOME}" "${REPO_ROOT}" avx2 >/dev/null 2>&1; then
    fail "home directory was accepted as an artifact root"
fi
HOME=${ORIGINAL_HOME}

echo "HIL helper tests passed"
