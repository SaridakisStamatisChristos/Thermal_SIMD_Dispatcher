#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
# shellcheck source=ci/hil-common.sh
source "${SCRIPT_DIR}/hil-common.sh"

BUILD_DIR=${BUILD_DIR:-"${REPO_ROOT}/build-hil"}
ARTIFACT_ROOT=${HIL_ARTIFACT_DIR:-"${REPO_ROOT}/hil-artifacts"}
SOAK_MINUTES=${SOAK_MINUTES:-30}
SAMPLE_INTERVAL_SECONDS=${SAMPLE_INTERVAL_SECONDS:-1}
METRICS_PORT=${METRICS_PORT:-19464}
WORK_ITEMS=${HIL_WORK_ITEMS:-1048576}
CHUNK_ITEMS=${HIL_CHUNK_ITEMS:-65536}
WORK_ROUNDS=${HIL_WORK_ROUNDS:-32}
BENCHMARK_TRIALS=${HIL_BENCHMARK_TRIALS:-5}
BENCHMARK_SECONDS=${HIL_BENCHMARK_SECONDS:-10}
BENCHMARK_WARMUP_SECONDS=${HIL_BENCHMARK_WARMUP_SECONDS:-1}
BENCHMARK_COOLDOWN_SECONDS=${HIL_BENCHMARK_COOLDOWN_SECONDS:-2}
BENCHMARK_SAMPLE_INTERVAL_SECONDS=${HIL_BENCHMARK_SAMPLE_INTERVAL_SECONDS:-0.1}
PRE_SOAK_COOLDOWN_SECONDS=${HIL_PRE_SOAK_COOLDOWN_SECONDS:-10}
ADAPTIVE_WARMUP_SECONDS=${HIL_ADAPTIVE_WARMUP_SECONDS:-2}

if ! [[ "${SOAK_MINUTES}" =~ ^[0-9]+$ ]] || [ "${SOAK_MINUTES}" -lt 1 ] || [ "${SOAK_MINUTES}" -gt 300 ]; then
    echo "SOAK_MINUTES must be an integer in [1, 300]" >&2
    exit 2
fi

TARGET_ISA=$(hil_target_isa)
EXPECTED_WIDTH=$(hil_expected_width "${TARGET_ISA}")
RUNTIME_ISA_ARGUMENT=$(hil_runtime_isa_argument "${TARGET_ISA}")
export HIL_TARGET_ISA METRICS_PORT
"${SCRIPT_DIR}/hil-preflight.sh"

if ! [[ "${PRE_SOAK_COOLDOWN_SECONDS}" =~ ^[0-9]+$ ]] || [ "${PRE_SOAK_COOLDOWN_SECONDS}" -gt 600 ]; then
    echo "HIL_PRE_SOAK_COOLDOWN_SECONDS must be an integer in [0, 600]" >&2
    exit 2
fi
python3 - \
    "${SAMPLE_INTERVAL_SECONDS}" "${WORK_ITEMS}" "${CHUNK_ITEMS}" "${WORK_ROUNDS}" \
    "${BENCHMARK_TRIALS}" "${BENCHMARK_SECONDS}" "${BENCHMARK_WARMUP_SECONDS}" \
    "${BENCHMARK_COOLDOWN_SECONDS}" "${ADAPTIVE_WARMUP_SECONDS}" \
    "${BENCHMARK_SAMPLE_INTERVAL_SECONDS}" <<'PY'
import math
import sys

try:
    sample_interval = float(sys.argv[1])
    work_items = int(sys.argv[2])
    chunk_items = int(sys.argv[3])
    rounds = int(sys.argv[4])
    trials = int(sys.argv[5])
    duration = float(sys.argv[6])
    warmup = float(sys.argv[7])
    cooldown = float(sys.argv[8])
    adaptive_warmup = float(sys.argv[9])
    benchmark_sample_interval = float(sys.argv[10])
except (ValueError, IndexError) as exc:
    raise SystemExit(f"invalid HIL numeric setting: {exc}") from exc

floats = (
    sample_interval,
    duration,
    warmup,
    cooldown,
    adaptive_warmup,
    benchmark_sample_interval,
)
if not all(math.isfinite(value) for value in floats):
    raise SystemExit("HIL duration/interval settings must be finite")
if not 0.05 <= sample_interval <= 60.0:
    raise SystemExit("SAMPLE_INTERVAL_SECONDS must be in [0.05, 60]")
if work_items < 16 or chunk_items < 16 or chunk_items > work_items:
    raise SystemExit("HIL work/chunk item counts must be at least 16 with chunk <= work")
if work_items % 16 or chunk_items % 16:
    raise SystemExit("HIL work/chunk item counts must be multiples of 16")
if not 1 <= rounds <= 10000:
    raise SystemExit("HIL_WORK_ROUNDS must be in [1, 10000]")
if not 2 <= trials <= 50:
    raise SystemExit("HIL_BENCHMARK_TRIALS must be in [2, 50]")
if not 0.05 <= duration <= 3600.0:
    raise SystemExit("HIL_BENCHMARK_SECONDS must be in [0.05, 3600]")
if not 0.0 <= warmup <= 300.0 or not 0.0 <= adaptive_warmup <= 3600.0:
    raise SystemExit("HIL warmup duration is outside its supported range")
if not 0.0 <= cooldown <= 600.0:
    raise SystemExit("HIL_BENCHMARK_COOLDOWN_SECONDS must be in [0, 600]")
if not 0.05 <= benchmark_sample_interval <= 1.0:
    raise SystemExit("HIL_BENCHMARK_SAMPLE_INTERVAL_SECONDS must be in [0.05, 1]")
PY

ARTIFACT_DIR=$(hil_prepare_run_directory "${ARTIFACT_ROOT}" "${REPO_ROOT}" "${TARGET_ISA}")
SOAK_SECONDS=$((SOAK_MINUTES * 60))
echo "HIL run artifacts: ${ARTIFACT_DIR}"

METADATA="${ARTIFACT_DIR}/machine-metadata.txt"
RUNTIME_LOG="${ARTIFACT_DIR}/runtime.log"
TIMELINE="${ARTIFACT_DIR}/timeline.csv"
HEALTH_JSONL="${ARTIFACT_DIR}/health.jsonl"
SUMMARY="${ARTIFACT_DIR}/summary.json"
METRICS_FINAL="${ARTIFACT_DIR}/metrics-final.prom"
PREFLIGHT_HEALTH="${ARTIFACT_DIR}/preflight-health.json"
ADAPTIVE_RESULT="${ARTIFACT_DIR}/adaptive-workload.json"
CONTROLLED_SUMMARY="${ARTIFACT_DIR}/controlled-benchmark.json"
CONTROLLED_CSV="${ARTIFACT_DIR}/controlled-benchmark.csv"
CONTROLLED_LOG="${ARTIFACT_DIR}/controlled-benchmark.log"
CONTROLLED_LOG_DIR="${ARTIFACT_DIR}/controlled-trials"

{
    echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "git_sha=$(git -C "${REPO_ROOT}" rev-parse HEAD)"
    echo "kernel=$(uname -srmo)"
    echo "target_isa=${TARGET_ISA}"
    echo "expected_width=${EXPECTED_WIDTH}"
    echo "runtime_isa_argument=${RUNTIME_ISA_ARGUMENT}"
    echo "runtime_workload=registered_integer_mix"
    echo "runtime_mode=persistent"
    echo "soak_minutes=${SOAK_MINUTES}"
    echo "sample_interval_seconds=${SAMPLE_INTERVAL_SECONDS}"
    echo "work_items_per_pass=${WORK_ITEMS}"
    echo "chunk_items=${CHUNK_ITEMS}"
    echo "rounds_per_item=${WORK_ROUNDS}"
    echo "controlled_trials_per_mode=${BENCHMARK_TRIALS}"
    echo "controlled_duration_seconds=${BENCHMARK_SECONDS}"
    echo "controlled_warmup_seconds=${BENCHMARK_WARMUP_SECONDS}"
    echo "controlled_cooldown_seconds=${BENCHMARK_COOLDOWN_SECONDS}"
    echo "controlled_sample_interval_seconds=${BENCHMARK_SAMPLE_INTERVAL_SECONDS}"
    echo "pre_soak_cooldown_seconds=${PRE_SOAK_COOLDOWN_SECONDS}"
    echo "adaptive_warmup_seconds=${ADAPTIVE_WARMUP_SECONDS}"
    echo "artifact_directory=${ARTIFACT_DIR}"
    echo "allowed_affinity=$(taskset -pc $$ 2>/dev/null || true)"
    echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unavailable)"
    echo "nmi_watchdog=$(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null || echo unavailable)"
    echo
    echo "=== toolchain ==="
    cmake --version | head -1 || true
    "${CC:-cc}" --version | head -1 || true
    "${CXX:-c++}" --version | head -1 || true
    python3 --version || true
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

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DTSD_BUILD_BENCHMARKS=ON
cmake --build "${BUILD_DIR}" --target thermal_simd benchmark_registered_kernel -j"$(nproc)"

python3 "${SCRIPT_DIR}/controlled_benchmark.py" \
    --executable "${BUILD_DIR}/benchmark_registered_kernel" \
    --target-isa "${TARGET_ISA}" \
    --trials "${BENCHMARK_TRIALS}" \
    --duration-seconds "${BENCHMARK_SECONDS}" \
    --warmup-seconds "${BENCHMARK_WARMUP_SECONDS}" \
    --cooldown-seconds "${BENCHMARK_COOLDOWN_SECONDS}" \
    --sample-interval-seconds "${BENCHMARK_SAMPLE_INTERVAL_SECONDS}" \
    --work-items "${WORK_ITEMS}" \
    --chunk-items "${CHUNK_ITEMS}" \
    --rounds "${WORK_ROUNDS}" \
    --output-json "${CONTROLLED_SUMMARY}" \
    --output-csv "${CONTROLLED_CSV}" \
    --log-dir "${CONTROLLED_LOG_DIR}" \
    >"${CONTROLLED_LOG}" 2>&1 || {
        echo "controlled registered-kernel benchmark failed" >&2
        tail -100 "${CONTROLLED_LOG}" >&2 || true
        exit 1
    }

if [ "${PRE_SOAK_COOLDOWN_SECONDS}" -gt 0 ]; then
    sleep "${PRE_SOAK_COOLDOWN_SECONDS}"
fi

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
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"${BUILD_DIR}/benchmark_registered_kernel" \
    --mode=adaptive \
    --max-isa="${TARGET_ISA}" \
    --run-forever \
    --warmup-seconds="${ADAPTIVE_WARMUP_SECONDS}" \
    --work-items="${WORK_ITEMS}" \
    --chunk-items="${CHUNK_ITEMS}" \
    --rounds="${WORK_ROUNDS}" \
    --metrics-bind=127.0.0.1 \
    --metrics-port="${METRICS_PORT}" \
    --result-json="${ADAPTIVE_RESULT}" \
    >"${RUNTIME_LOG}" 2>&1 &
RUNTIME_PID=$!
echo "runtime_pid=${RUNTIME_PID}" >> "${METADATA}"

HEALTH_URL="http://127.0.0.1:${METRICS_PORT}/healthz"
READY_URL="http://127.0.0.1:${METRICS_PORT}/readyz"
METRICS_URL="http://127.0.0.1:${METRICS_PORT}/metrics"
READY=0
for _ in $(seq 1 150); do
    if curl --max-time 1 --silent --fail "${READY_URL}" >/dev/null 2>&1; then
        READY=1
        break
    fi
    if ! kill -0 "${RUNTIME_PID}" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if [ "${READY}" -ne 1 ]; then
    echo "adaptive runtime did not become ready; perf and thermal/frequency telemetry are required" >&2
    curl --max-time 1 --silent "${HEALTH_URL}" >&2 || true
    tail -100 "${RUNTIME_LOG}" >&2 || true
    exit 1
fi
curl --max-time 2 --silent --fail "${HEALTH_URL}" > "${PREFLIGHT_HEALTH}"

python3 "${SCRIPT_DIR}/hil_sampler.py" \
    --url "${HEALTH_URL}" \
    --duration-seconds "${SOAK_SECONDS}" \
    --interval-seconds "${SAMPLE_INTERVAL_SECONDS}" \
    --csv "${TIMELINE}" \
    --jsonl "${HEALTH_JSONL}" \
    --summary "${SUMMARY}"

curl --max-time 2 --silent --fail "${METRICS_URL}" > "${METRICS_FINAL}" || true

if ! kill -0 "${RUNTIME_PID}" 2>/dev/null; then
    echo "adaptive registered workload exited before the characterization window completed" >&2
    tail -100 "${RUNTIME_LOG}" >&2 || true
    exit 1
fi

kill -TERM "${RUNTIME_PID}"
set +e
wait "${RUNTIME_PID}"
RUNTIME_STATUS=$?
set -e
RUNTIME_PID=""
if [ "${RUNTIME_STATUS}" -ne 0 ]; then
    echo "adaptive registered workload did not shut down cleanly after SIGTERM (status=${RUNTIME_STATUS})" >&2
    tail -100 "${RUNTIME_LOG}" >&2 || true
    exit 1
fi
echo "graceful_sigterm_shutdown=true" >> "${METADATA}"

python3 - "${SUMMARY}" "${EXPECTED_WIDTH}" "${ADAPTIVE_RESULT}" "${CONTROLLED_SUMMARY}" <<'PY'
import json
import sys
from pathlib import Path

summary = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
expected_width = sys.argv[2]
adaptive = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
controlled = json.loads(Path(sys.argv[4]).read_text(encoding="utf-8"))
errors = []
if summary.get("sample_count", 0) < 10:
    errors.append("too few samples")
if summary.get("health_sample_fraction", 0.0) < 0.95:
    errors.append("health endpoint availability below 95%")
if summary.get("live_fraction", 0.0) < 0.95:
    errors.append("runtime liveness below 95%")
if summary.get("ready_fraction", 0.0) < 0.90:
    errors.append("strict runtime readiness below 90%")
if summary.get("hardware_perf_fraction", 0.0) < 0.90:
    errors.append("hardware perf counters healthy for less than 90% of samples")
if summary.get("temperature_sample_fraction", 0.0) < 0.90:
    errors.append("package temperature available for less than 90% of samples")
width_samples = summary.get("width_samples") or {}
if not width_samples:
    errors.append("no SIMD-width observations")
if width_samples.get(expected_width, 0) < 1:
    errors.append(f"requested width {expected_width} was never observed")
if adaptive.get("mode") != "adaptive" or adaptive.get("completed_passes", 0) < 1:
    errors.append("adaptive registered workload produced no complete measured pass")
if not adaptive.get("stopped_by_signal", False):
    errors.append("adaptive registered workload did not record graceful signal termination")
adaptive_width_work = adaptive.get("width_work_items") or {}
if adaptive_width_work.get(expected_width, 0) < 1:
    errors.append(f"adaptive application kernel never executed as {expected_width}")
expected_checksum = controlled.get("expected_checksum_fnv1a64")
if not expected_checksum or adaptive.get("checksum_fnv1a64") != expected_checksum:
    errors.append("adaptive/fixed registered-kernel checksum mismatch")
required_modes = ["sse41", "avx2"] + (["avx512"] if expected_width == "avx512" else [])
for mode in required_modes:
    aggregate = (controlled.get("modes") or {}).get(mode) or {}
    if aggregate.get("trial_count", 0) < 2:
        errors.append(f"missing repeated fixed-width control for {mode}")
if errors:
    for error in errors:
        print(f"HIL validation failure: {error}", file=sys.stderr)
    raise SystemExit(1)
PY

python3 - "${SUMMARY}" "${ADAPTIVE_RESULT}" "${CONTROLLED_SUMMARY}" > "${ARTIFACT_DIR}/summary.md" <<'PY'
import json
import sys
from pathlib import Path

s = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
adaptive = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
controlled = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
print("# Thermal SIMD HIL characterization")
print()
print(f"- Target ISA: **{controlled['target_isa']}**")
print(f"- Samples: **{s['sample_count']}**")
print(f"- Duration: **{s['duration_seconds']:.1f} s**")
print(f"- Health availability: **{100*s['health_sample_fraction']:.2f}%**")
print(f"- Runtime liveness: **{100*s['live_fraction']:.2f}%**")
print(f"- Strict readiness: **{100*s['ready_fraction']:.2f}%**")
print(f"- Hardware perf healthy: **{100*s['hardware_perf_fraction']:.2f}%**")
print(f"- Temperature coverage: **{100*s['temperature_sample_fraction']:.2f}%**")
print(f"- Width samples: `{s['width_samples']}`")
print(f"- Adaptive width work items: `{adaptive['width_work_items']}`")
print(f"- Adaptive checksum: `{adaptive['checksum_fnv1a64']}`")
print("- Graceful SIGTERM shutdown: **validated**")
if s.get("max_temperature_c") is not None:
    print(f"- Max package temperature: **{s['max_temperature_c']:.2f} °C**")
if s.get("mean_temperature_c") is not None:
    print(f"- Mean package temperature: **{s['mean_temperature_c']:.2f} °C**")
if s.get("mean_rapl_power_w") is not None:
    print(f"- RAPL package energy: **{s['rapl_energy_j']:.2f} J**")
    print(f"- Mean RAPL package power: **{s['mean_rapl_power_w']:.2f} W**")
    print(f"- Max sampled RAPL package power: **{s['max_rapl_power_w']:.2f} W**")
else:
    print("- RAPL package energy/power: **unavailable on this runner**")
print()
print("## Fixed-width registered-kernel controls")
print()
print("| Mode | Trials | Median items/s | MAD items/s | Median package W | Median items/J |")
print("| --- | ---: | ---: | ---: | ---: | ---: |")
for mode, aggregate in controlled["modes"].items():
    power = aggregate.get("median_package_power_w")
    efficiency = aggregate.get("median_items_per_joule")
    power_text = f"{power:.3f}" if power is not None else "unavailable"
    efficiency_text = f"{efficiency:.3f}" if efficiency is not None else "unavailable"
    print(
        f"| {mode} | {aggregate['trial_count']} | {aggregate['median_items_per_second']:.3f} | "
        f"{aggregate['throughput_mad_items_per_second']:.3f} | {power_text} | {efficiency_text} |"
    )
print()
for comparison, values in controlled.get("comparisons", {}).items():
    ratio = values.get("median_throughput_ratio")
    efficiency = values.get("median_efficiency_ratio")
    if ratio is not None:
        print(f"- {comparison} median throughput ratio: **{ratio:.4f}×**")
    if efficiency is not None:
        print(f"- {comparison} median efficiency ratio: **{efficiency:.4f}×**")
PY

cat "${ARTIFACT_DIR}/summary.md"
echo "[thermal-characterization] completed ${SOAK_MINUTES} minute ${TARGET_ISA} HIL evidence run"
