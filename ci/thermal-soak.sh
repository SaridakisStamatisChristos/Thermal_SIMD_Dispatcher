#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${REPO_ROOT}/build-hil"}
ARTIFACT_DIR=${HIL_ARTIFACT_DIR:-"${REPO_ROOT}/hil-artifacts"}
SOAK_MINUTES=${SOAK_MINUTES:-30}
SAMPLE_INTERVAL_SECONDS=${SAMPLE_INTERVAL_SECONDS:-1}
METRICS_PORT=${METRICS_PORT:-19464}
WORK_ITERS=${WORK_ITERS:-1000000}

if ! [[ "${SOAK_MINUTES}" =~ ^[0-9]+$ ]] || [ "${SOAK_MINUTES}" -lt 1 ] || [ "${SOAK_MINUTES}" -gt 300 ]; then
    echo "SOAK_MINUTES must be an integer in [1, 300]" >&2
    exit 2
fi
if ! lscpu | grep -Eq '(^|[[:space:]])avx512f([[:space:]]|$)'; then
    echo "thermal characterization requires an AVX-512F-capable HIL runner" >&2
    exit 1
fi

SOAK_SECONDS=$((SOAK_MINUTES * 60))
# Give endpoint startup and final sampling a small margin while keeping the
# runtime's --duration-sec contract itself wall-clock accurate.
RUNTIME_SECONDS=$((SOAK_SECONDS + 30))
rm -rf "${ARTIFACT_DIR}"
mkdir -p "${ARTIFACT_DIR}"

METADATA="${ARTIFACT_DIR}/machine-metadata.txt"
RUNTIME_LOG="${ARTIFACT_DIR}/runtime.log"
TIMELINE="${ARTIFACT_DIR}/timeline.csv"
HEALTH_JSONL="${ARTIFACT_DIR}/health.jsonl"
SUMMARY="${ARTIFACT_DIR}/summary.json"
METRICS_FINAL="${ARTIFACT_DIR}/metrics-final.prom"

{
    echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "git_sha=$(git -C "${REPO_ROOT}" rev-parse HEAD)"
    echo "kernel=$(uname -srmo)"
    echo "soak_minutes=${SOAK_MINUTES}"
    echo "runtime_seconds=${RUNTIME_SECONDS}"
    echo "sample_interval_seconds=${SAMPLE_INTERVAL_SECONDS}"
    echo "work_batch_iterations=${WORK_ITERS}"
    echo "avx512_requested=true"
    echo "allowed_affinity=$(taskset -pc $$ 2>/dev/null || true)"
    echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unavailable)"
    echo "nmi_watchdog=$(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null || echo unavailable)"
    echo
    echo "=== lscpu ==="
    lscpu || true
    echo
    echo "=== cpu model / microcode ==="
    grep -m1 -E 'model name|Hardware' /proc/cpuinfo || true
    grep -m1 '^microcode' /proc/cpuinfo || true
    echo
    echo "=== CPU package topology ==="
    for path in /sys/devices/system/cpu/cpu[0-9]*/topology/physical_package_id; do
        [ -r "${path}" ] || continue
        printf '%s=' "${path}"
        cat "${path}"
    done
    echo
    echo "=== cpufreq governors ==="
    for path in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
        [ -r "${path}" ] || continue
        printf '%s=' "${path}"
        cat "${path}"
    done
    echo
    echo "=== thermal sensors ==="
    for path in /sys/class/thermal/thermal_zone*/type /sys/class/hwmon/hwmon*/name; do
        [ -r "${path}" ] || continue
        printf '%s=' "${path}"
        cat "${path}"
    done
    echo
    echo "=== powercap domains ==="
    for path in /sys/class/powercap/*/name; do
        [ -r "${path}" ] || continue
        printf '%s=' "${path}"
        cat "${path}"
    done
    echo
    echo "=== /dev/cpu MSR visibility ==="
    find /dev/cpu -maxdepth 2 -name msr -print 2>/dev/null || true
} > "${METADATA}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "${BUILD_DIR}" --target thermal_simd -j"$(nproc)"

RUNTIME_PID=""
cleanup() {
    if [ -n "${RUNTIME_PID}" ] && kill -0 "${RUNTIME_PID}" 2>/dev/null; then
        kill -TERM "${RUNTIME_PID}" 2>/dev/null || true
        for _ in $(seq 1 20); do
            if ! kill -0 "${RUNTIME_PID}" 2>/dev/null; then
                break
            fi
            sleep 0.1
        done
        if kill -0 "${RUNTIME_PID}" 2>/dev/null; then
            kill -KILL "${RUNTIME_PID}" 2>/dev/null || true
        fi
    fi
}
trap cleanup EXIT INT TERM

"${BUILD_DIR}/thermal_simd" \
    --allow-avx512 \
    --duration-sec="${RUNTIME_SECONDS}" \
    --work-iters="${WORK_ITERS}" \
    --metrics-bind=127.0.0.1 \
    --metrics-port="${METRICS_PORT}" \
    >"${RUNTIME_LOG}" 2>&1 &
RUNTIME_PID=$!

echo "runtime_pid=${RUNTIME_PID}" >> "${METADATA}"

HEALTH_URL="http://127.0.0.1:${METRICS_PORT}/healthz"
METRICS_URL="http://127.0.0.1:${METRICS_PORT}/metrics"
READY=0
for _ in $(seq 1 100); do
    if curl --max-time 1 --silent --fail "${HEALTH_URL}" >/dev/null 2>&1; then
        READY=1
        break
    fi
    if ! kill -0 "${RUNTIME_PID}" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if [ "${READY}" -ne 1 ]; then
    echo "thermal-simd health endpoint did not become live" >&2
    tail -100 "${RUNTIME_LOG}" >&2 || true
    exit 1
fi

python3 "${SCRIPT_DIR}/hil_sampler.py" \
    --url "${HEALTH_URL}" \
    --duration-seconds "${SOAK_SECONDS}" \
    --interval-seconds "${SAMPLE_INTERVAL_SECONDS}" \
    --csv "${TIMELINE}" \
    --jsonl "${HEALTH_JSONL}" \
    --summary "${SUMMARY}"

curl --max-time 2 --silent --fail "${METRICS_URL}" > "${METRICS_FINAL}" || true

if kill -0 "${RUNTIME_PID}" 2>/dev/null; then
    kill -TERM "${RUNTIME_PID}" 2>/dev/null || true
    wait "${RUNTIME_PID}" 2>/dev/null || true
else
    echo "thermal-simd exited before the characterization window completed" >&2
fi
RUNTIME_PID=""

python3 - "${SUMMARY}" <<'PY'
import json
import sys
from pathlib import Path

summary = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
errors = []
if summary.get("sample_count", 0) < 10:
    errors.append("too few samples")
if summary.get("health_sample_fraction", 0.0) < 0.95:
    errors.append("health endpoint availability below 95%")
if summary.get("live_fraction", 0.0) < 0.95:
    errors.append("runtime liveness below 95%")
if summary.get("hardware_perf_fraction", 0.0) < 0.90:
    errors.append("hardware perf counters healthy for less than 90% of samples")
if summary.get("temperature_sample_fraction", 0.0) < 0.90:
    errors.append("package temperature available for less than 90% of samples")
width_samples = summary.get("width_samples") or {}
if not width_samples:
    errors.append("no SIMD-width observations")
if width_samples.get("avx512", 0) < 1:
    errors.append("AVX-512 was explicitly requested but never observed")
if errors:
    for error in errors:
        print(f"HIL validation failure: {error}", file=sys.stderr)
    raise SystemExit(1)
PY

python3 - "${SUMMARY}" > "${ARTIFACT_DIR}/summary.md" <<'PY'
import json
import sys
from pathlib import Path

s = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
print("# Thermal SIMD HIL characterization")
print()
print(f"- Samples: **{s['sample_count']}**")
print(f"- Duration: **{s['duration_seconds']:.1f} s**")
print(f"- Health availability: **{100*s['health_sample_fraction']:.2f}%**")
print(f"- Runtime liveness: **{100*s['live_fraction']:.2f}%**")
print(f"- Ready: **{100*s['ready_fraction']:.2f}%**")
print(f"- Hardware perf healthy: **{100*s['hardware_perf_fraction']:.2f}%**")
print(f"- Temperature coverage: **{100*s['temperature_sample_fraction']:.2f}%**")
print(f"- Width samples: `{s['width_samples']}`")
if s.get('max_temperature_c') is not None:
    print(f"- Max package temperature: **{s['max_temperature_c']:.2f} °C**")
if s.get('mean_temperature_c') is not None:
    print(f"- Mean package temperature: **{s['mean_temperature_c']:.2f} °C**")
if s.get('mean_rapl_power_w') is not None:
    print(f"- Mean RAPL package power: **{s['mean_rapl_power_w']:.2f} W**")
    print(f"- Max RAPL package power: **{s['max_rapl_power_w']:.2f} W**")
else:
    print("- RAPL package power: **unavailable on this runner**")
PY

cat "${ARTIFACT_DIR}/summary.md"
echo "[thermal-characterization] completed ${SOAK_MINUTES} minute HIL evidence run"
