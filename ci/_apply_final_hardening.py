#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, text):
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def regex_once(text, pattern, repl, label):
    out, count = re.subn(pattern, repl, text, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError(f"{label}: expected one regex match, found {count}")
    return out


# ---------------------------------------------------------------------------
# Configuration: pure initialization + runtime-only degraded overlay.
# ---------------------------------------------------------------------------
p = "src/thermal_config.c"
s = read(p)
s = replace_once(
    s,
    """void tsd_runtime_config_set_defaults(tsd_runtime_config *cfg) {\n    if (!cfg) return;\n    *cfg = k_default_config;\n    cfg->policy = k_default_policy_config;\n    tsd_policy_config_apply_bounds(&cfg->policy);\n    cfg->degraded_policy_active = 0;\n    atomic_store_explicit(&g_tsd_degraded_active, 0, memory_order_release);\n}\n\nint tsd_runtime_config_is_degraded(void) {""",
    """void tsd_runtime_config_set_defaults(tsd_runtime_config *cfg) {\n    if (!cfg) return;\n    *cfg = k_default_config;\n    cfg->policy = k_default_policy_config;\n    tsd_policy_config_apply_bounds(&cfg->policy);\n    cfg->degraded_policy_active = 0;\n}\n\nvoid tsd_runtime_config_reset_dynamic_state(void) {\n    atomic_store_explicit(&g_tsd_degraded_active, 0, memory_order_release);\n}\n\nint tsd_runtime_config_is_degraded(void) {""",
    "pure config defaults",
)
s = replace_once(
    s,
    """void tsd_runtime_config_enter_degraded_mode(tsd_runtime_config *cfg, const char *reason) {\n    if (!cfg) return;""",
    """void tsd_runtime_config_enter_degraded_mode(tsd_runtime_config *cfg, const char *reason) {\n    if (!cfg || cfg != &g_tsd_config) return;""",
    "degraded enter ownership",
)
s = replace_once(
    s,
    """void tsd_runtime_config_exit_degraded_mode(tsd_runtime_config *cfg, const char *reason) {\n    if (!cfg) return;""",
    """void tsd_runtime_config_exit_degraded_mode(tsd_runtime_config *cfg, const char *reason) {\n    if (!cfg || cfg != &g_tsd_config) return;""",
    "degraded exit ownership",
)
write(p, s)


# ---------------------------------------------------------------------------
# Built-in shim + lifecycle: coherent selection, admission, owner domain,
# effective degraded policy, and bounded shutdown quiescence.
# ---------------------------------------------------------------------------
p = "src/thermal_simd.c"
s = read(p)
s = replace_once(s, "#include <time.h>\n#include <unistd.h>",
                 "#include <time.h>\n#include <sys/syscall.h>\n#include <unistd.h>",
                 "thermal_simd syscall include")

shim = r'''/*
 * The built-in shim consumes one coherent {width,target} snapshot. No lock is
 * held across executable user/application work; wide admission is accounted by
 * the same in-flight protocol used by registered kernels.
 */
__attribute__((naked))
static int32_t simd_shim_unlocked(int32_t a __attribute__((unused)),
                                  int32_t b __attribute__((unused)),
                                  tsd_patch_slot_t *target __attribute__((unused)),
                                  simd_width_t selected __attribute__((unused))) {
    __asm__ __volatile__(
        "cmpb $0, g_tsd_avx_available(%rip)\n\t"
        "je 1f\n\t"
        "testl %ecx, %ecx\n\t"
        "jne 1f\n\t"
        ".byte 0xC5, 0xF8, 0x77\n\t"
        "1:\n\t"
        "movd %edi, %xmm0\n\t"
        "movd %esi, %xmm1\n\t"
        "call *%rdx\n\t"
        "movd %xmm0, %eax\n\t"
        "ret\n\t"
    );
}

static int32_t simd_shim(int32_t a, int32_t b) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        tsd_trampoline_selection_t snapshot = {0};
        if (tsd_trampoline_state_snapshot(&snapshot) != 0 || !snapshot.active) return 0;
        simd_width_t selected = snapshot.width;

#ifdef TSD_ENABLE_TESTS
        int enter_rc = 0;
#else
        int enter_rc = tsd_runtime_execution_enter(selected);
#endif
        if (enter_rc == 0) {
            int32_t result = simd_shim_unlocked(a, b, snapshot.active, selected);
#ifndef TSD_ENABLE_TESTS
            tsd_runtime_execution_leave(selected);
#endif
            return result;
        }
        if (selected <= SIMD_SSE41 || errno != EAGAIN) return 0;

        /* Admission/authorization changed after the snapshot. Repair the
         * process selector to the conservative implementation and retry. */
        if (tsd_trampoline_patch(SIMD_SSE41) != 0) return 0;
    }
    return 0;
}

static void workload_loop'''
s = regex_once(
    s,
    r'/\*\n \* The naked shim is only entered.*?\nstatic void workload_loop',
    shim,
    "coherent built-in shim",
)

# Degraded runtime policy must actually drive monitor timing/count thresholds.
for old, new in [
    ("g_tsd_config.down_count", "tsd_runtime_config_effective_down_count(&g_tsd_config)"),
    ("g_tsd_config.up_count", "tsd_runtime_config_effective_up_count(&g_tsd_config)"),
    ("g_tsd_config.cooldown_down_ms", "tsd_runtime_config_effective_cooldown_down_ms(&g_tsd_config)"),
    ("g_tsd_config.cooldown_up_ms", "tsd_runtime_config_effective_cooldown_up_ms(&g_tsd_config)"),
    ("g_tsd_config.min_dwell_ms", "tsd_runtime_config_effective_min_dwell_ms(&g_tsd_config)"),
]:
    s = s.replace(old, new)

s = s.replace("atomic_load_explicit(&g_tsd_current_width, memory_order_acquire)",
              "tsd_trampoline_state_current_width()")

# No normal optimization transition without a real performance observation.
s = replace_once(
    s,
    """        if (emergency) {\n            evaluation_rc = 1;\n            if (eval.severity_milli == 0) eval.severity_milli = 1;\n        }\n\n        /* Dwell/cooldown control only normal optimization transitions. */""",
    """        if (emergency) {\n            evaluation_rc = 1;\n            if (eval.severity_milli == 0) eval.severity_milli = 1;\n        }\n\n        /* Lack of measured owner-thread work/performance is not evidence of\n         * stability. Safety telemetry above still runs every tick, but normal\n         * policy transitions wait for an actual performance observation. */\n        if (!eval.performance_available) {\n            stable_count = 0;\n            continue;\n        }\n\n        /* Dwell/cooldown control only normal optimization transitions. */""",
    "idle performance gate",
)

# Runtime start begins with admission closed and no owner until the monitor is live.
s = replace_once(
    s,
    """    tsd_runtime_set_stopping_locked(0);\n    tsd_runtime_safety_write_leave();""",
    """    tsd_runtime_set_stopping_locked(1);\n    tsd_runtime_set_owner_tid_locked(0);\n    tsd_runtime_config_reset_dynamic_state();\n    tsd_runtime_safety_write_leave();""",
    "runtime startup gate closed",
)

# Publish owner identity only after the monitor thread exists; roll back if publication fails.
s = replace_once(
    s,
    """    runtime->monitor_started = 1;\n    g_tsd_active_runtime = runtime;\n    *out_runtime = runtime;""",
    """    runtime->monitor_started = 1;\n    if (tsd_runtime_safety_write_enter() != 0) {\n        atomic_store_explicit(&g_tsd_running, 0, memory_order_release);\n        pthread_join(runtime->monitor, NULL);\n        runtime->monitor_started = 0;\n        tsd_perf_cleanup(runtime->perf);\n        free(runtime);\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        return -1;\n    }\n    tsd_runtime_set_owner_tid_locked((int)syscall(SYS_gettid));\n    tsd_runtime_set_stopping_locked(0);\n    tsd_runtime_safety_write_leave();\n\n    g_tsd_active_runtime = runtime;\n    *out_runtime = runtime;""",
    "runtime owner publication",
)

# Blocking stop from inside an admitted callback is rejected before side effects.
s = replace_once(
    s,
    """int tsd_runtime_stop(tsd_runtime_t *runtime) {\n    if (!runtime) {\n        errno = EINVAL;\n        return -1;\n    }\n\n    pthread_mutex_lock(&g_tsd_runtime_lock);""",
    """int tsd_runtime_stop(tsd_runtime_t *runtime) {\n    if (!runtime) {\n        errno = EINVAL;\n        return -1;\n    }\n    if (tsd_runtime_current_thread_in_wide_execution()) {\n        errno = EDEADLK;\n        return -1;\n    }\n\n    pthread_mutex_lock(&g_tsd_runtime_lock);""",
    "reentrant stop rejection",
)

s = replace_once(
    s,
    """    if (runtime->monitor_started) {\n        pthread_join(runtime->monitor, NULL);\n        runtime->monitor_started = 0;\n    }\n\n    /* Successful shutdown requires the conservative selection to be committed.""",
    """    if (runtime->monitor_started) {\n        pthread_join(runtime->monitor, NULL);\n        runtime->monitor_started = 0;\n    }\n\n    /* Admission was closed before the monitor stopped. Drain only invocations\n     * that were already admitted; new wide work cannot join this set. */\n    if (tsd_runtime_wait_for_wide_quiescence() != 0) {\n        int saved_errno = errno ? errno : EIO;\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        errno = saved_errno;\n        return -1;\n    }\n\n    /* Successful shutdown requires the conservative selection to be committed.""",
    "shutdown quiescence",
)

s = replace_once(
    s,
    """    if (tsd_runtime_safety_write_enter() == 0) {\n        tsd_runtime_set_stopping_locked(0);\n        tsd_runtime_safety_write_leave();\n    }""",
    """    if (tsd_runtime_safety_write_enter() == 0) {\n        tsd_runtime_set_owner_tid_locked(0);\n        tsd_runtime_set_stopping_locked(0);\n        tsd_runtime_config_reset_dynamic_state();\n        tsd_runtime_safety_write_leave();\n    }""",
    "shutdown guard reset",
)
write(p, s)


# ---------------------------------------------------------------------------
# Perf controller: revoke before fallback, owner-thread cycles/work objective,
# and explicit performance-validity publication.
# ---------------------------------------------------------------------------
p = "src/thermal_perf.c"
s = read(p)
s = replace_once(s, "#include <thermal/simd/thermal_trampoline.h>\n",
                 "#include <thermal/simd/thermal_trampoline.h>\n\n#include \"runtime_guard_internal.h\"\n",
                 "perf guard include")

s = replace_once(
    s,
    """    uint64_t baseline_cpi;\n    uint64_t calibrated_cpi_reference;\n    uint64_t baseline_llc_mpki_milli;""",
    """    uint64_t baseline_cpi;\n    uint64_t calibrated_cpi_reference;\n    uint64_t baseline_work_cost_milli;\n    uint64_t calibrated_work_reference;\n    uint64_t baseline_llc_mpki_milli;""",
    "perf work references",
)
s = replace_once(
    s,
    """    uint64_t sw_last_iterations;\n    tsd_workload_fn workload;""",
    """    uint64_t sw_last_iterations;\n    uint64_t hw_last_iterations;\n    tsd_workload_fn workload;""",
    "perf hardware work cursor",
)

# Software fallback is a revocation transaction: close admission and publish
# unhealthy/software authority before attempting physical SSE selection.
s = replace_once(
    s,
    """    if (mode == TSD_PERF_MODE_SOFTWARE) {\n        /* Hardware CPI is not comparable to software ns/work-item. */\n        ctx->calibrated_cpi_reference = 0;\n        ctx->hardware_validated = 0;\n        tsd_metrics_increment(TSD_METRIC_PERF_FALLBACKS);\n        tsd_runtime_config_enter_degraded_mode(&g_tsd_config, why);""",
    """    if (mode == TSD_PERF_MODE_SOFTWARE) {\n        /* Revoke admission before changing any physical selector. The guard\n         * publication below is authoritative; SSE selection is then cleanup. */\n        tsd_runtime_wide_admission_close();\n        ctx->calibrated_cpi_reference = 0;\n        ctx->calibrated_work_reference = 0;\n        ctx->hardware_validated = 0;\n        tsd_metrics_increment(TSD_METRIC_PERF_FALLBACKS);\n        tsd_runtime_config_enter_degraded_mode(&g_tsd_config, why);\n        publish_perf_state(ctx, 0);""",
    "perf fail-close ordering",
)
s = replace_once(
    s,
    """        publish_perf_state(ctx, 0);\n    } else if (mode == TSD_PERF_MODE_HARDWARE) {""",
    """    } else if (mode == TSD_PERF_MODE_HARDWARE) {""",
    "remove duplicate software publish",
)
s = s.replace("atomic_load_explicit(&g_tsd_current_width, memory_order_acquire)",
              "tsd_trampoline_state_current_width()")

# Hardware baseline also captures completed work to calibrate cycles/work when
# the embedder supplied a workload callback. Registered-kernel runtimes lazily
# calibrate on their first owner-thread work sample while still in SSE mode.
s = replace_once(
    s,
    """    create_baseline_observation(ctx);\n\n    if (tsd_perf_read_exact(ctx->fd_cycles, &rd_after, sizeof(rd_after)) != 0 || rd_after.nr != 2) {""",
    """    uint64_t work_before = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);\n    create_baseline_observation(ctx);\n    uint64_t work_after = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);\n\n    if (tsd_perf_read_exact(ctx->fd_cycles, &rd_after, sizeof(rd_after)) != 0 || rd_after.nr != 2) {""",
    "baseline work window",
)
s = replace_once(
    s,
    """    ctx->baseline_cpi = (delta_cycles * 1000) / delta_insns;\n    ctx->calibrated_cpi_reference = ctx->baseline_cpi ? ctx->baseline_cpi : 1;\n    uint64_t delta_llc =""",
    """    ctx->baseline_cpi = (delta_cycles * 1000) / delta_insns;\n    ctx->calibrated_cpi_reference = ctx->baseline_cpi ? ctx->baseline_cpi : 1;\n    uint64_t baseline_work = work_after >= work_before ? work_after - work_before : 0;\n    ctx->baseline_work_cost_milli = baseline_work\n        ? (uint64_t)(((__uint128_t)delta_cycles * 1000u) / baseline_work)\n        : 0;\n    ctx->calibrated_work_reference = ctx->baseline_work_cost_milli;\n    ctx->hw_last_iterations = work_after;\n    uint64_t delta_llc =""",
    "baseline cycles per work",
)

# Extend process_measurement with a work-normalized control cost.
s = replace_once(
    s,
    """static int process_measurement(perf_ctx_t *ctx, tsd_thermal_eval_t *out, uint64_t current_cpi,\n                               uint64_t mpki_milli, const tsd_runtime_config *cfg,""",
    """static int process_measurement(perf_ctx_t *ctx, tsd_thermal_eval_t *out, uint64_t current_cpi,\n                               uint64_t current_work_cost_milli, uint64_t mpki_milli,\n                               const tsd_runtime_config *cfg,""",
    "process measurement signature",
)

old_ratio = """    uint64_t reference_cpi = ctx->slow_cpi ? ctx->slow_cpi : (ctx->baseline_cpi ? ctx->baseline_cpi : 1);\n    __uint128_t ratio_num = (__uint128_t)current_cpi * 1000u;\n    uint64_t adaptive_ratio_milli = (uint64_t)((ratio_num + reference_cpi / 2) / reference_cpi);\n    uint64_t absolute_reference = ctx->calibrated_cpi_reference ? ctx->calibrated_cpi_reference : reference_cpi;\n    uint64_t absolute_ratio_milli = (uint64_t)((ratio_num + absolute_reference / 2) / absolute_reference);\n    /* A slow EWMA must not normalize sustained degradation back to 1.0. Keep a\n     * frozen live calibration and use the more conservative of the two ratios. */\n    uint64_t ratio_milli = adaptive_ratio_milli > absolute_ratio_milli\n                               ? adaptive_ratio_milli : absolute_ratio_milli;"""
new_ratio = """    uint64_t reference_cpi = ctx->slow_cpi ? ctx->slow_cpi : (ctx->baseline_cpi ? ctx->baseline_cpi : 1);\n    __uint128_t cpi_ratio_num = (__uint128_t)current_cpi * 1000u;\n    uint64_t adaptive_ratio_milli = (uint64_t)((cpi_ratio_num + reference_cpi / 2) / reference_cpi);\n    uint64_t absolute_reference = ctx->calibrated_cpi_reference ? ctx->calibrated_cpi_reference : reference_cpi;\n    uint64_t absolute_ratio_milli = (uint64_t)((cpi_ratio_num + absolute_reference / 2) / absolute_reference);\n    uint64_t cpi_ratio_milli = adaptive_ratio_milli > absolute_ratio_milli\n                                   ? adaptive_ratio_milli : absolute_ratio_milli;\n\n    uint64_t ratio_milli = cpi_ratio_milli;\n    if (current_work_cost_milli > 0) {\n        if (ctx->calibrated_work_reference == 0) {\n            ctx->calibrated_work_reference = current_work_cost_milli;\n            ctx->baseline_work_cost_milli = current_work_cost_milli;\n        }\n        __uint128_t work_ratio_num = (__uint128_t)current_work_cost_milli * 1000u;\n        ratio_milli = (uint64_t)((work_ratio_num + ctx->calibrated_work_reference / 2) /\n                                 ctx->calibrated_work_reference);\n    }"""
s = replace_once(s, old_ratio, new_ratio, "work normalized performance ratio")

s = replace_once(
    s,
    """    if (out) {\n        out->cpi_milli = current_cpi;""",
    """    if (out) {\n        out->performance_available = 1;\n        out->work_normalized = current_work_cost_milli > 0 ? 1 : 0;\n        out->work_cost_milli = current_work_cost_milli;\n        out->cpi_milli = current_cpi;""",
    "performance validity output",
)

# Calls from deterministic script/software have no separate hardware cycles/work channel.
s = s.replace("process_measurement(ctx, out, current_cpi, g_test_perf_script.mpki, cfg, &scripted_telemetry)",
              "process_measurement(ctx, out, current_cpi, 0, g_test_perf_script.mpki, cfg, &scripted_telemetry)")
s = s.replace("process_measurement(ctx, out, software_cost_milli, 0, cfg, &telemetry)",
              "process_measurement(ctx, out, software_cost_milli, 0, 0, cfg, &telemetry)")

# Hardware sample denominator is completed owner-thread work in the same window.
s = replace_once(
    s,
    """    uint64_t current_cpi = (delta_cycles * 1000) / delta_insns;\n    uint64_t delta_llc =""",
    """    uint64_t current_cpi = (delta_cycles * 1000) / delta_insns;\n    uint64_t now_work = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);\n    uint64_t delta_work = now_work >= ctx->hw_last_iterations ? now_work - ctx->hw_last_iterations : 0;\n    ctx->hw_last_iterations = now_work;\n    uint64_t current_work_cost_milli = delta_work\n        ? (uint64_t)(((__uint128_t)delta_cycles * 1000u) / delta_work)\n        : 0;\n    uint64_t delta_llc =""",
    "hardware work cost",
)
s = replace_once(
    s,
    "return process_measurement(ctx, out, current_cpi, mpki_milli, cfg, &telemetry);",
    "return process_measurement(ctx, out, current_cpi, current_work_cost_milli, mpki_milli, cfg, &telemetry);",
    "hardware process call",
)
write(p, s)


# ---------------------------------------------------------------------------
# Fusion bridge source identity.
# ---------------------------------------------------------------------------
p = "src/telemetry/fusion_bridge.cpp"
s = read(p)
s = replace_once(
    s,
    """        reading.quality = 100;\n        reading.timestamp = now;\n        bus->publish(telemetry::TelemetrySignal::kPackageTempC, reading);""",
    """        reading.quality = 100;\n        reading.timestamp = now;\n        reading.source = \"direct\";\n        bus->publish(telemetry::TelemetrySignal::kPackageTempC, reading);""",
    "bridge temp source",
)
s = replace_once(
    s,
    """        reading.quality = 100;\n        reading.timestamp = now;\n        bus->publish(telemetry::TelemetrySignal::kFrequencyRatio, reading);""",
    """        reading.quality = 100;\n        reading.timestamp = now;\n        reading.source = \"direct\";\n        bus->publish(telemetry::TelemetrySignal::kFrequencyRatio, reading);""",
    "bridge freq source",
)
write(p, s)


# ---------------------------------------------------------------------------
# Multi-package telemetry: unlabeled package sensors are not safety authority.
# ---------------------------------------------------------------------------
p = "src/telemetry_helper.c"
s = read(p)
package_count_fn = r'''
static int physical_package_count(void) {
    DIR *dir = opendir("/sys/devices/system/cpu");
    if (!dir) return 0;
    int packages[256];
    size_t count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cpu", 3) != 0) continue;
        const char *digits = entry->d_name + 3;
        if (*digits < '0' || *digits > '9') continue;
        char *end = NULL;
        long cpu = strtol(digits, &end, 10);
        if (!end || *end != '\0' || cpu < 0 || cpu > 1048576) continue;
        int package = physical_package_id((int)cpu);
        if (package < 0) continue;
        int seen = 0;
        for (size_t i = 0; i < count; ++i) {
            if (packages[i] == package) { seen = 1; break; }
        }
        if (!seen && count < sizeof(packages) / sizeof(packages[0])) packages[count++] = package;
    }
    closedir(dir);
    return (int)count;
}
'''
s = replace_once(
    s,
    """static int label_mentions_package(const char *label, int package_id) {""",
    package_count_fn + "\nstatic int label_mentions_package(const char *label, int package_id) {",
    "package count helper",
)
s = replace_once(
    s,
    """    if (best.score < 0 || best.path[0] == '\\0') return 0;\n\n    /*\n     * On multi-package hosts an unlabeled top-scoring package sensor is not a""",
    """    if (best.score < 0 || best.path[0] == '\\0') return 0;\n\n    const int package_count = physical_package_count();\n    if (package_count > 1 && (package_id < 0 || !best.explicit_package_match)) {\n        tsd_log_warn(LOG_COMPONENT,\n                     \"multi-package host requires explicit package-labelled thermal authority; cpu=%d package=%d packages=%d\",\n                     helper->cpu, package_id, package_count);\n        return 0;\n    }\n\n    /*\n     * On multi-package hosts an unlabeled top-scoring package sensor is not a""",
    "multi-package fail closed",
)
write(p, s)


# ---------------------------------------------------------------------------
# Trampoline attestation logging: no iostream allocation on C-facing hot path.
# ---------------------------------------------------------------------------
p = "src/patcher/trampoline.cpp"
s = read(p)
s = replace_once(
    s,
    """    std::ostringstream oss;\n    oss << \"Trampoline hash=\";\n    for (uint8_t byte : g_active_hash) {\n        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);\n    }\n    tsd_log_info(LOG_COMPONENT, \"%s\", oss.str().c_str());""",
    """    char hash_line[32 + TSD_ATTESTATION_HASH_SIZE * 2] = {0};\n    int used = std::snprintf(hash_line, sizeof(hash_line), \"Trampoline hash=\");\n    if (used < 0) return;\n    size_t cursor = static_cast<size_t>(used);\n    for (uint8_t byte : g_active_hash) {\n        if (cursor + 2 >= sizeof(hash_line)) break;\n        int written = std::snprintf(hash_line + cursor, sizeof(hash_line) - cursor, \"%02x\",\n                                    static_cast<unsigned int>(byte));\n        if (written != 2) break;\n        cursor += 2;\n    }\n    tsd_log_info(LOG_COMPONENT, \"%s\", hash_line);""",
    "allocation-free attestation log",
)
write(p, s)


# ---------------------------------------------------------------------------
# Runtime lifecycle regressions: revocation must not wait for a running kernel,
# callback selector reentry must not deadlock, and owner-domain resolution must
# fail closed for non-owner threads.
# ---------------------------------------------------------------------------
p = "tests/runtime/test_runtime_lifecycle.c"
s = read(p)
s = replace_once(s, "#include <stdlib.h>\n#include <unistd.h>",
                 "#include <stdlib.h>\n#include <sys/syscall.h>\n#include <time.h>\n#include <unistd.h>",
                 "runtime test syscall include")
s = replace_once(s, "#include <thermal/simd/thermal_trampoline.h>\n",
                 "#include <thermal/simd/thermal_trampoline.h>\n\n#include \"runtime_guard_internal.h\"\n",
                 "runtime guard test include")

s = replace_once(
    s,
    """    usleep(5000);\n    /* Guard publication takes the write side and therefore cannot complete\n       while the non-preemptible kernel still holds the execution side. */\n    assert(atomic_load_explicit(&kernel.revocation_done, memory_order_acquire) == 0);\n\n    pthread_mutex_lock(&kernel.mutex);""",
    """    for (int i = 0; i < 100 &&\n                    !atomic_load_explicit(&kernel.revocation_done, memory_order_acquire); ++i) {\n        usleep(1000);\n    }\n    /* Revocation closes admission immediately and must not wait for an already\n       admitted non-preemptible callback to finish. */\n    assert(atomic_load_explicit(&kernel.revocation_done, memory_order_acquire) == 1);\n    simd_width_t during = SIMD_AVX2;\n    assert(tsd_kernel_dispatch_resolve(dispatch, &during) == 0);\n    assert(during == SIMD_SSE41);\n\n    pthread_mutex_lock(&kernel.mutex);""",
    "nonblocking revocation expectation",
)

insert_tests = r'''
typedef struct {
    _Atomic int done;
    int patch_rc;
} reentrant_kernel_ctx_t;

static void reentrant_selector_kernel(void *opaque, size_t work_items) {
    (void)work_items;
    reentrant_kernel_ctx_t *ctx = (reentrant_kernel_ctx_t *)opaque;
    ctx->patch_rc = tsd_trampoline_patch(SIMD_SSE41);
    atomic_store_explicit(&ctx->done, 1, memory_order_release);
}

static void *execute_reentrant(void *opaque) {
    execute_ctx_t *ctx = (execute_ctx_t *)opaque;
    ctx->rc = tsd_kernel_dispatch_execute(ctx->dispatch, 1, &ctx->used);
    return NULL;
}

static void test_callback_selector_reentrancy(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    tsd_runtime_flags_init();
    tsd_runtime_flags_record_sandbox_success();
    publish_hardware_guard(60.0);
    assert(tsd_trampoline_init() == 0);

    tsd_runtime_config probe = g_tsd_config;
    probe.allow_avx512 = 1;
    if (tsd_detect_max_simd(&probe) < SIMD_AVX2) {
        clear_observability_guard();
        return;
    }
    assert(tsd_trampoline_patch(SIMD_AVX2) == 0);

    reentrant_kernel_ctx_t kernel = {.patch_rc = -1};
    atomic_init(&kernel.done, 0);
    tsd_kernel_variants_t variants = {
        .sse41 = reentrant_selector_kernel,
        .avx2 = reentrant_selector_kernel,
        .avx512 = reentrant_selector_kernel,
        .context = &kernel,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);

    execute_ctx_t exec = {.dispatch = dispatch, .used = SIMD_SSE41, .rc = -1};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, execute_reentrant, &exec) == 0);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    assert(pthread_timedjoin_np(thread, NULL, &deadline) == 0);
    assert(exec.rc == 0);
    assert(exec.used == SIMD_AVX2);
    assert(atomic_load_explicit(&kernel.done, memory_order_acquire) == 1);
    assert(kernel.patch_rc == 0);
    assert(tsd_trampoline_state_current_width() == SIMD_SSE41);
    tsd_kernel_dispatch_destroy(dispatch);
    clear_observability_guard();
}

typedef struct {
    tsd_kernel_dispatch_t *dispatch;
    _Atomic int stop;
} reader_loop_ctx_t;

static void no_op_kernel(void *opaque, size_t work_items) {
    (void)opaque;
    (void)work_items;
}

static void *dispatch_reader_loop(void *opaque) {
    reader_loop_ctx_t *ctx = (reader_loop_ctx_t *)opaque;
    while (!atomic_load_explicit(&ctx->stop, memory_order_acquire)) {
        simd_width_t used = SIMD_SSE41;
        assert(tsd_kernel_dispatch_execute(ctx->dispatch, 1, &used) == 0);
    }
    return NULL;
}

static void test_revocation_not_starved_by_readers(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    tsd_runtime_flags_init();
    tsd_runtime_flags_record_sandbox_success();
    publish_hardware_guard(60.0);
    assert(tsd_trampoline_init() == 0);

    tsd_runtime_config probe = g_tsd_config;
    probe.allow_avx512 = 1;
    if (tsd_detect_max_simd(&probe) < SIMD_AVX2) {
        clear_observability_guard();
        return;
    }
    assert(tsd_trampoline_patch(SIMD_AVX2) == 0);

    tsd_kernel_variants_t variants = {
        .sse41 = no_op_kernel,
        .avx2 = no_op_kernel,
        .avx512 = no_op_kernel,
        .context = NULL,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);
    reader_loop_ctx_t readers = {.dispatch = dispatch};
    atomic_init(&readers.stop, 0);
    pthread_t threads[8];
    for (size_t i = 0; i < 8; ++i) assert(pthread_create(&threads[i], NULL, dispatch_reader_loop, &readers) == 0);

    blocking_kernel_ctx_t revocation = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    atomic_init(&revocation.revocation_done, 0);
    pthread_t revoker;
    assert(pthread_create(&revoker, NULL, revoke_temperature, &revocation) == 0);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    assert(pthread_timedjoin_np(revoker, NULL, &deadline) == 0);
    assert(atomic_load_explicit(&revocation.revocation_done, memory_order_acquire) == 1);

    atomic_store_explicit(&readers.stop, 1, memory_order_release);
    for (size_t i = 0; i < 8; ++i) assert(pthread_join(threads[i], NULL) == 0);
    tsd_kernel_dispatch_destroy(dispatch);
    assert(tsd_trampoline_patch(SIMD_SSE41) == 0);
    clear_observability_guard();
}

typedef struct {
    tsd_kernel_dispatch_t *dispatch;
    simd_width_t resolved;
    int rc;
} resolve_thread_ctx_t;

static void *resolve_on_other_thread(void *opaque) {
    resolve_thread_ctx_t *ctx = (resolve_thread_ctx_t *)opaque;
    ctx->resolved = SIMD_AVX2;
    ctx->rc = tsd_kernel_dispatch_resolve(ctx->dispatch, &ctx->resolved);
    return NULL;
}

static void test_owner_domain_fail_closed(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    tsd_runtime_flags_init();
    tsd_runtime_flags_record_sandbox_success();
    publish_hardware_guard(60.0);
    assert(tsd_trampoline_init() == 0);

    tsd_runtime_config probe = g_tsd_config;
    probe.allow_avx512 = 1;
    if (tsd_detect_max_simd(&probe) < SIMD_AVX2) {
        clear_observability_guard();
        return;
    }
    assert(tsd_trampoline_patch(SIMD_AVX2) == 0);
    assert(tsd_runtime_safety_write_enter() == 0);
    tsd_runtime_set_owner_tid_locked((int)syscall(SYS_gettid));
    tsd_runtime_safety_write_leave();

    tsd_kernel_variants_t variants = {
        .sse41 = no_op_kernel,
        .avx2 = no_op_kernel,
        .avx512 = no_op_kernel,
        .context = NULL,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);
    simd_width_t local = SIMD_SSE41;
    assert(tsd_kernel_dispatch_resolve(dispatch, &local) == 0);
    assert(local == SIMD_AVX2);

    resolve_thread_ctx_t other = {.dispatch = dispatch, .resolved = SIMD_AVX2, .rc = -1};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, resolve_on_other_thread, &other) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(other.rc == 0);
    assert(other.resolved == SIMD_SSE41);

    assert(tsd_runtime_safety_write_enter() == 0);
    tsd_runtime_set_owner_tid_locked(0);
    tsd_runtime_safety_write_leave();
    tsd_kernel_dispatch_destroy(dispatch);
    assert(tsd_trampoline_patch(SIMD_SSE41) == 0);
    clear_observability_guard();
}

'''
s = replace_once(s, "int main(void) {\n    test_execution_revocation_linearization();",
                 insert_tests + "int main(void) {\n    test_execution_revocation_linearization();\n    test_callback_selector_reentrancy();\n    test_revocation_not_starved_by_readers();\n    test_owner_domain_fail_closed();",
                 "insert runtime admission regressions")
write(p, s)


# ---------------------------------------------------------------------------
# Config regression: local initialization cannot clear live degraded state.
# ---------------------------------------------------------------------------
p = "tests/config/test_runtime_config_cli.c"
s = read(p)
config_test = r'''
static void test_config_defaults_are_pure(void) {
    tsd_runtime_config_reset_dynamic_state();
    tsd_runtime_config_set_defaults(&g_tsd_config);
    tsd_runtime_config_enter_degraded_mode(&g_tsd_config, "test");
    assert(tsd_runtime_config_is_degraded());

    tsd_runtime_config local;
    tsd_runtime_config_set_defaults(&local);
    assert(tsd_runtime_config_is_degraded());
    tsd_runtime_config_enter_degraded_mode(&local, "must-not-own-global-state");
    assert(tsd_runtime_config_is_degraded());
    tsd_runtime_config_exit_degraded_mode(&local, "must-not-own-global-state");
    assert(tsd_runtime_config_is_degraded());

    tsd_runtime_config_exit_degraded_mode(&g_tsd_config, "test-done");
    assert(!tsd_runtime_config_is_degraded());
}

'''
s = replace_once(s, "int main(void) {\n    test_cli_predictive_and_metrics();",
                 config_test + "int main(void) {\n    test_config_defaults_are_pure();\n    test_cli_predictive_and_metrics();",
                 "config purity test")
write(p, s)


# ---------------------------------------------------------------------------
# Policy fixtures mark performance as real observations.
# ---------------------------------------------------------------------------
p = "tests/policy/test_policy_controller.c"
s = read(p)
s = replace_once(s, "    tsd_thermal_eval_t sample = {0};\n    sample.ratio_milli = ratio_milli;",
                 "    tsd_thermal_eval_t sample = {0};\n    sample.performance_available = 1;\n    sample.ratio_milli = ratio_milli;",
                 "policy performance validity")
write(p, s)


# ---------------------------------------------------------------------------
# TSan covers interruptible fusion as well as stress fusion.
# ---------------------------------------------------------------------------
p = ".github/workflows/quality.yml"
s = read(p)
s = replace_once(
    s,
    """            test_telemetry_fusion_stress \\\n            test_observability_metrics""",
    """            test_telemetry_fusion \\\n            test_telemetry_fusion_stress \\\n            test_observability_metrics""",
    "TSan fusion target",
)
s = replace_once(
    s,
    '"^(telemetry_fusion_stress|observability_metrics|adaptive_dispatch|runtime_lifecycle)$"',
    '"^(telemetry_fusion_thread|telemetry_fusion_stress|observability_metrics|adaptive_dispatch|runtime_lifecycle)$"',
    "TSan fusion regex",
)
write(p, s)

print("final hardening transformations applied")
