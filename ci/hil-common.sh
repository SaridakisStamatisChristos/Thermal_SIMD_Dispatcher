#!/usr/bin/env bash

# Shared helpers for Linux hardware-in-the-loop entrypoints.  This file is
# sourced by other scripts; do not enable or change the caller's shell options.

hil_target_isa() {
    local target=${HIL_TARGET_ISA:-avx2}
    case "${target}" in
        avx2|avx512)
            printf '%s\n' "${target}"
            ;;
        *)
            echo "HIL_TARGET_ISA must be either avx2 or avx512 (got: ${target})" >&2
            return 2
            ;;
    esac
}

hil_required_cpu_flag() {
    case "$1" in
        avx2) printf '%s\n' avx2 ;;
        avx512) printf '%s\n' avx512f ;;
        *) return 2 ;;
    esac
}

hil_runtime_isa_argument() {
    case "$1" in
        avx2) printf '%s\n' --no-avx512 ;;
        avx512) printf '%s\n' --allow-avx512 ;;
        *) return 2 ;;
    esac
}

hil_expected_width() {
    case "$1" in
        avx2) printf '%s\n' avx2 ;;
        avx512) printf '%s\n' avx512 ;;
        *) return 2 ;;
    esac
}

hil_require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "HIL prerequisite missing: $1" >&2
        return 1
    fi
}

hil_require_target_isa() {
    local target=$1
    local required_flag
    required_flag=$(hil_required_cpu_flag "${target}") || return
    if ! lscpu | grep -Eq "(^|[[:space:]])${required_flag}([[:space:]]|$)"; then
        echo "HIL target ${target} requires CPU flag ${required_flag}, but the host does not expose it" >&2
        return 1
    fi
}

hil_prepare_run_directory() {
    local requested_root=$1
    local repo_root=$2
    local target=$3
    local root run_id run_dir home_dir

    if [ -z "${requested_root}" ]; then
        echo "HIL artifact root must not be empty" >&2
        return 2
    fi

    # Creating a directory is safe; canonicalize only after it exists.  Unlike
    # the old harness, this function never recursively deletes caller-selected
    # paths.  Every invocation writes into a new, validated child directory.
    mkdir -p -- "${requested_root}"
    root=$(cd -- "${requested_root}" && pwd -P) || return
    repo_root=$(cd -- "${repo_root}" && pwd -P) || return
    home_dir=""
    if [ -n "${HOME:-}" ] && [ -d "${HOME}" ]; then
        home_dir=$(cd -- "${HOME}" && pwd -P) || home_dir=""
    fi

    if [ "${root}" = "/" ] || [ "${root}" = "${repo_root}" ] || { [ -n "${home_dir}" ] && [ "${root}" = "${home_dir}" ]; }; then
        echo "refusing unsafe HIL artifact root: ${root}" >&2
        return 2
    fi

    run_id=${HIL_RUN_ID:-"$(date -u +%Y%m%dT%H%M%SZ)-${target}-$$"}
    if ! [[ "${run_id}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
        echo "HIL_RUN_ID may contain only letters, digits, dot, underscore and hyphen" >&2
        return 2
    fi

    run_dir="${root}/${run_id}"
    if [ -e "${run_dir}" ]; then
        echo "refusing to reuse existing HIL run directory: ${run_dir}" >&2
        return 2
    fi
    mkdir -- "${run_dir}"
    printf '%s\n' "${run_dir}"
}
