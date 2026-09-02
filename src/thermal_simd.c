#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <config/runtime_flags.h>
#include <healthcheck/sandbox.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_cpu.h>
#include <thermal/simd/thermal_perf.h>
#include <thermal/simd/thermal_signals.h>
#include <thermal/simd/thermal_trampoline.h>
#include <thermal/simd/logging.h>
#include <thermal/simd/health_check.h>
#include <thermal/simd/policy/dispatcher_policy.h>
#include <observability/metrics_exporter.h>

#ifdef TSD_ENABLE_TESTS
#include "thermal_simd_test.h"
#endif

static _Atomic int g_tsd_running = 1;
static _Atomic int g_tsd_reload_requested = 0;

static void tsd_handle_sighup(int sig) {
    (void)sig;
    atomic_store_explicit(&g_tsd_reload_requested, 1, memory_order_relaxed);
}

static int32_t simd_shim(int32_t a, int32_t b);

static inline void workload_once(void) {
    (void)simd_shim(42, 7);
    atomic_fetch_add_explicit(&g_tsd_workload_iterations, 1, memory_order_relaxed);
}

static uint64_t compute_dwell_ms_from_ticks(int dwell_ticks) {
    if (dwell_ticks <= 0) {
        return 0;
    }
    uint64_t interval_us = (uint64_t)g_tsd_config.check_interval_us;
    return (interval_us * (uint64_t)dwell_ticks) / 1000ULL;
}

static void record_dwell_metric(simd_width_t width, int dwell_ticks) {
    uint64_t dwell_ms = compute_dwell_ms_from_ticks(dwell_ticks);
    if (dwell_ms > 0) {
        tsd_metrics_exporter_observe_dwell(width, dwell_ms);
    }
}

static int temperature_upgrade_allowed(const tsd_thermal_eval_t *eval) {
    if (!eval || !eval->temp_available) {
        return 0;
    }
    if (g_tsd_config.predictive_temp_ceiling_c < 20 ||
        g_tsd_config.predictive_temp_ceiling_c > 125 ||
        g_tsd_config.predictive_safety_margin_c < 0) {
        return 1;
    }
    int64_t limit = (int64_t)(g_tsd_config.predictive_temp_ceiling_c -
                              g_tsd_config.predictive_safety_margin_c) * 1000;
    return (int64_t)eval->package_temp_millic <= limit;
}

static int emergency_temperature_exceeded(const tsd_thermal_eval_t *eval) {
    if (!eval || !eval->temp_available) {
        return 0;
    }
    if (g_tsd_config.predictive_temp_ceiling_c < 20 ||
        g_tsd_config.predictive_temp_ceiling_c > 125 ||
        g_tsd_config.predictive_emergency_margin_c < 0) {
        return 0;
    }
    int64_t limit = (int64_t)(g_tsd_config.predictive_temp_ceiling_c +
                              g_tsd_config.predictive_emergency_margin_c) * 1000;
    return (int64_t)eval->package_temp_millic >= limit;
}

__attribute__((naked))
static int32_t simd_shim(int32_t a __attribute__((unused)),
                         int32_t b __attribute__((unused))) {
    __asm__ __volatile__(
        "cmpb $0, g_tsd_avx_available(%rip)\n\t"
        "je 1f\n\t"
        "cmpb $0, g_tsd_current_width_byte(%rip)\n\t"
        "jne 1f\n\t"
        ".byte 0xC5, 0xF8, 0x77\n\t"
        "1:\n\t"
        "movd %edi, %xmm0\n\t"
        "movd %esi, %xmm1\n\t"
        "movq g_tsd_active_trampoline(%rip), %rax\n\t"
        "call *%rax\n\t"
        "movd %xmm0, %eax\n\t"
        "ret\n\t"
    );
}

static void workload_loop(int iterations) {
    for (int i = 0; i < iterations; ++i) {
        workload_once();
    }
}

typedef struct {
    uint64_t iterations;
    uint64_t elapsed_ns;
} tsd_demo_result_t;

static uint64_t monotonic_elapsed_ns(const struct timespec *start, const struct timespec *end) {
    if (!start || !end) {
        return 0;
    }
    time_t sec = end->tv_sec - start->tv_sec;
    long nsec = end->tv_nsec - start->tv_nsec;
    if (nsec < 0) {
        sec -= 1;
        nsec += 1000000000L;
    }
    if (sec < 0) {
        return 0;
    }
    return (uint64_t)sec * UINT64_C(1000000000) + (uint64_t)nsec;
}

static tsd_demo_result_t run_workload_for_duration(int duration_sec, int batch_iterations) {
    tsd_demo_result_t result = {0};
    if (duration_sec <= 0 || batch_iterations <= 0) {
        return result;
    }

    struct timespec start = {0};
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        tsd_log_error("runtime", "CLOCK_MONOTONIC unavailable; cannot honor --duration-sec");
        return result;
    }

    uint64_t before = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
    const uint64_t target_ns = (uint64_t)duration_sec * UINT64_C(1000000000);
    do {
        workload_loop(batch_iterations);
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            break;
        }
        result.elapsed_ns = monotonic_elapsed_ns(&start, &now);
    } while (result.elapsed_ns < target_ns);

    if (result.elapsed_ns == 0 && clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        result.elapsed_ns = monotonic_elapsed_ns(&start, &now);
    }
    uint64_t after = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
    result.iterations = after - before;
    return result;
}

static void log_demo_result(const tsd_demo_result_t *result) {
    if (!result) {
        return;
    }
    uint64_t throughput = 0;
    if (result->elapsed_ns > 0) {
        throughput = (uint64_t)(((__uint128_t)result->iterations * UINT64_C(1000000000)) /
                                result->elapsed_ns);
    }
    tsd_log_info("runtime",
                 "Workload demo complete: elapsed_ms=%" PRIu64 " iterations=%" PRIu64
                 " throughput_iter_per_sec=%" PRIu64,
                 result->elapsed_ns / UINT64_C(1000000), result->iterations, throughput);
}

#define LOG_COMPONENT "runtime"

#ifdef TSD_ENABLE_TESTS
#define TSD_MAYBE_UNUSED __attribute__((unused))
#else
#define TSD_MAYBE_UNUSED
#endif

static void print_configuration(simd_width_t max_width) TSD_MAYBE_UNUSED;
static void print_configuration(simd_width_t max_width) {
    const char *max_width_str = (max_width == SIMD_AVX512)
        ? "AVX-512 (512-bit)"
        : (max_width == SIMD_AVX2 ? "AVX2 (256-bit)" : "SSE4.1 (128-bit)");
    tsd_log_info(LOG_COMPONENT, "Maximum supported: %s%s",
                 max_width_str,
                 g_tsd_config.allow_avx512 ? "" : " [AVX-512 disabled by policy]");
    tsd_log_info(LOG_COMPONENT, "AVX transition guard: %s",
                 g_tsd_avx_available ? "enabled" : "disabled");
    tsd_log_info(LOG_COMPONENT, "Policy configuration:");
    tsd_log_info(LOG_COMPONENT, "  Check interval: %d ms", g_tsd_config.check_interval_us / 1000);
    tsd_log_info(LOG_COMPONENT, "  Down threshold: %.1fx CPI (after %d events)",
                 g_tsd_config.down_ratio, g_tsd_config.down_count);
    tsd_log_info(LOG_COMPONENT, "  Up threshold: %d stable events", g_tsd_config.up_count);
    tsd_log_info(LOG_COMPONENT, "  Cooldown: %d ms down, %d ms up",
                 g_tsd_config.cooldown_down_ms, g_tsd_config.cooldown_up_ms);
    tsd_log_info(LOG_COMPONENT, "  Minimum dwell: %d ms per width", g_tsd_config.min_dwell_ms);
    tsd_log_info(LOG_COMPONENT, "  Memory guard: divisor=%d offset=%d milli",
                 g_tsd_config.memory_guard_divisor, g_tsd_config.memory_guard_offset_milli);
    tsd_log_info(LOG_COMPONENT, "  Telemetry weights: temp=%d ratio=%d (milli)",
                 g_tsd_config.thermal_temp_weight_milli, g_tsd_config.thermal_ratio_weight_milli);
    tsd_log_info(LOG_COMPONENT, "  Cooldown ticks: down=%d up=%d min-dwell=%d",
                 g_tsd_config.cooldown_down_ticks,
                 g_tsd_config.cooldown_up_ticks,
                 g_tsd_config.min_dwell_ticks);
    tsd_log_info(LOG_COMPONENT, "  Demo: %d wall-clock sec, work batch: %d iterations",
                 g_tsd_config.demo_duration_sec, g_tsd_config.work_iters);
}

static void append_message(char *buf, size_t buf_size, size_t *cursor, const char *fmt, ...) {
    if (!buf || buf_size == 0 || !cursor || *cursor >= buf_size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + *cursor, buf_size - *cursor, fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    if ((size_t)written >= buf_size - *cursor) {
        *cursor = buf_size - 1;
        buf[*cursor] = '\0';
    } else {
        *cursor += (size_t)written;
    }
}

static int evaluate_thermal(perf_ctx_t *ctx, tsd_thermal_eval_t *out) {
    return tsd_perf_evaluate(ctx, out, &g_tsd_config);
}

void* thermal_monitor_thread(void *arg) {
    perf_ctx_t *ctx = (perf_ctx_t*)arg;
    simd_width_t width = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
    simd_width_t max_width_cached = tsd_detect_max_simd(&g_tsd_config);
    int throttle_count = 0;
    int stable_count = 0;
    int cooldown = 0;
    int dwell_ticks = 0;
    tsd_dispatcher_policy_state *policy_state = tsd_dispatcher_policy_create(&g_tsd_config.policy);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET((size_t)tsd_perf_get_monitor_cpu(ctx), &cpuset);
    (void)sched_setaffinity(0, sizeof(cpuset), &cpuset);

    while (atomic_load_explicit(&g_tsd_running, memory_order_acquire)) {
        struct timespec interval = {
            .tv_sec = g_tsd_config.check_interval_us / 1000000,
            .tv_nsec = (long)(g_tsd_config.check_interval_us % 1000000) * 1000L,
        };
        while (nanosleep(&interval, &interval) == -1 && errno == EINTR) {}
        dwell_ticks++;

        /* Fail-closed selections can originate inside telemetry evaluation. */
        width = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);

        if (policy_state) {
            tsd_dispatcher_policy_heartbeat(policy_state, width);
            if (atomic_exchange_explicit(&g_tsd_reload_requested, 0, memory_order_relaxed)) {
                if (tsd_dispatcher_policy_reload(policy_state) != 0) {
                    tsd_log_warn(LOG_COMPONENT, "SIGHUP coefficient reload failed; retaining existing/fallback policy state");
                } else {
                    tsd_log_info(LOG_COMPONENT, "SIGHUP coefficient reload completed");
                }
            }
        }

        if (cooldown > 0) {
            cooldown--;
            continue;
        }
        if (dwell_ticks < g_tsd_config.min_dwell_ticks) {
            continue;
        }
        if (tsd_perf_check_software_timeout(ctx, g_tsd_config.degraded_timeout_sec)) {
            simd_width_t current_width = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
            record_dwell_metric(current_width, dwell_ticks);
            if (current_width != SIMD_SSE41) {
                if (tsd_trampoline_patch(SIMD_SSE41) == 0) {
                    width = SIMD_SSE41;
                    if (policy_state) {
                        tsd_dispatcher_policy_reset(policy_state, &g_tsd_config.policy);
                    }
                } else {
                    tsd_log_error(LOG_COMPONENT, "fail-closed patch to SSE4.1 failed");
                }
            } else {
                width = current_width;
            }
            cooldown = g_tsd_config.cooldown_down_ticks;
            dwell_ticks = 0;
            continue;
        }
        tsd_thermal_eval_t eval = {0};
        int predictive_fallback = 0;
        int evaluation_rc = evaluate_thermal(ctx, &eval);
        width = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
        if (emergency_temperature_exceeded(&eval)) {
            evaluation_rc = 1;
            if (eval.severity_milli == 0) {
                eval.severity_milli = 1;
            }
        }
        if (policy_state) {
            tsd_dispatcher_policy_record(policy_state, &eval, width);
        }

        if (cooldown <= 0 && dwell_ticks >= g_tsd_config.min_dwell_ticks && policy_state) {
            simd_width_t target = width;
            int used_predictive = tsd_dispatcher_policy_recommend(policy_state, width, max_width_cached,
                                                                  &target, &predictive_fallback);
            if (predictive_fallback && tsd_log_should_log(TSD_LOG_LEVEL_DEBUG)) {
                tsd_log_debug(LOG_COMPONENT, "Predictive controller unavailable; using hysteresis fallback");
            }
            if (used_predictive > 0 && target != width) {
                simd_width_t previous = width;
                if (target > previous &&
                    (!tsd_runtime_flags_allow_transitions() || !tsd_perf_upgrades_allowed(ctx) ||
                     !temperature_upgrade_allowed(&eval))) {
                    target = previous;
                } else if (!tsd_runtime_flags_allow_transitions() && target != SIMD_SSE41) {
                    target = SIMD_SSE41;
                }
                if (target != previous) {
                    record_dwell_metric(previous, dwell_ticks);
                    int patch_rc = tsd_trampoline_patch(target);
                    if (patch_rc == 0) {
                        width = target;
                        throttle_count = 0;
                        stable_count = 0;
                        dwell_ticks = 0;
                        cooldown = (target < previous) ? g_tsd_config.cooldown_down_ticks
                                                       : g_tsd_config.cooldown_up_ticks;
                        continue;
                    }
                    tsd_dispatcher_policy_force_fallback(policy_state);
                    if (tsd_log_should_log(TSD_LOG_LEVEL_WARN)) {
                        tsd_log_warn(LOG_COMPONENT, "Predictive patch failed; reverting to hysteresis policy");
                    }
                }
            }
        }

        if (evaluation_rc) {
            throttle_count++;
            stable_count = 0;
            if (throttle_count >= g_tsd_config.down_count && width > SIMD_SSE41) {
                char message[512] = {0};
                size_t cursor = 0;
                append_message(message, sizeof(message), &cursor,
                               "Thermal throttle: ratio=%u.%03u (trimmed %u.%03u) severity=+%lu.%03lu (thermal %lu.%03lu) MPKI=%lu.%03lu",
                               eval.ratio_milli / 1000, eval.ratio_milli % 1000,
                               eval.trimmed_ratio_milli / 1000, eval.trimmed_ratio_milli % 1000,
                               eval.severity_milli / 1000, eval.severity_milli % 1000,
                               eval.thermal_severity_milli / 1000, eval.thermal_severity_milli % 1000,
                               eval.llc_mpki_milli / 1000, eval.llc_mpki_milli % 1000);
                if (eval.temp_available) {
                    int32_t temp_whole = eval.package_temp_millic / 1000;
                    int32_t temp_frac = eval.package_temp_millic % 1000;
                    if (temp_frac < 0) {
                        temp_frac = -temp_frac;
                    }
                    append_message(message, sizeof(message), &cursor,
                                   " temp=%d.%03dC", temp_whole, temp_frac);
                }
                if (eval.freq_ratio_available) {
                    append_message(message, sizeof(message), &cursor,
                                   " freq=%u.%03ux",
                                   eval.freq_ratio_milli / 1000, eval.freq_ratio_milli % 1000);
                }
                if (eval.memory_bound) {
                    append_message(message, sizeof(message), &cursor,
                                   " [memory bound guard raised]");
                }
                tsd_log_warn(LOG_COMPONENT, "%s", message);
                simd_width_t target = width - 1;
                record_dwell_metric(width, dwell_ticks);
                if (!tsd_runtime_flags_allow_transitions() && target > SIMD_SSE41) {
                    target = SIMD_SSE41;
                }
                int patch_rc = tsd_trampoline_patch(target);
                if (patch_rc == 0) {
                    width = target;
                } else {
                    int patch_err = errno;
                    width = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
#ifdef TSD_ENABLE_TESTS
                    const char *patch_err_msg = tsd_trampoline_last_error();
                    if (patch_err_msg && patch_err_msg[0] != '\0') {
                        tsd_log_error(LOG_COMPONENT, "downgrade patch failed: %s", patch_err_msg);
                    } else
#endif
                    if (patch_err != 0) {
                        char errbuf[128];
                        tsd_log_error(LOG_COMPONENT, "downgrade patch failed: %s",
                                      tsd_log_strerror(patch_err, errbuf, sizeof(errbuf)));
                    } else {
                        tsd_log_error(LOG_COMPONENT, "downgrade patch failed");
                    }
                }
                throttle_count = 0;
                cooldown = g_tsd_config.cooldown_down_ticks;
                dwell_ticks = 0;
            }
        } else {
            stable_count++;
            throttle_count = 0;
            if (stable_count >= g_tsd_config.up_count && width < max_width_cached) {
                if (!tsd_runtime_flags_allow_transitions() || !tsd_perf_upgrades_allowed(ctx) ||
                    !temperature_upgrade_allowed(&eval)) {
                    stable_count = 0;
                    continue;
                }
                tsd_log_info(LOG_COMPONENT,
                             "Recovered: ratio=%u.%03u (trimmed %u.%03u) MPKI=%lu.%03lu",
                             eval.ratio_milli / 1000, eval.ratio_milli % 1000,
                             eval.trimmed_ratio_milli / 1000, eval.trimmed_ratio_milli % 1000,
                             eval.llc_mpki_milli / 1000, eval.llc_mpki_milli % 1000);
                simd_width_t target = width + 1;
                record_dwell_metric(width, dwell_ticks);
                int patch_rc = tsd_trampoline_patch(target);
                if (patch_rc == 0) {
                    width = target;
                } else {
                    int patch_err = errno;
                    width = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
#ifdef TSD_ENABLE_TESTS
                    const char *patch_err_msg = tsd_trampoline_last_error();
                    if (patch_err_msg && patch_err_msg[0] != '\0') {
                        tsd_log_error(LOG_COMPONENT, "upgrade patch failed: %s", patch_err_msg);
                    } else
#endif
                    if (patch_err != 0) {
                        char errbuf[128];
                        tsd_log_error(LOG_COMPONENT, "upgrade patch failed: %s",
                                      tsd_log_strerror(patch_err, errbuf, sizeof(errbuf)));
                    } else {
                        tsd_log_error(LOG_COMPONENT, "upgrade patch failed");
                    }
                }
                stable_count = 0;
                cooldown = g_tsd_config.cooldown_up_ticks;
                dwell_ticks = 0;
            }
        }
    }
    if (policy_state) {
        tsd_dispatcher_policy_destroy(policy_state);
    }
    return NULL;
}

static void run_demo(void) TSD_MAYBE_UNUSED;
static void run_demo(void) {
    atomic_store_explicit(&g_tsd_running, 1, memory_order_release);
    perf_ctx_t *perf = tsd_perf_init(workload_once);
    if (!perf) {
        tsd_log_warn(LOG_COMPONENT, "Performance monitoring unavailable; running without thermal adaptation.");
        tsd_log_info(LOG_COMPONENT, "Suggestions: run as root, adjust kernel.perf_event_paranoid, or add container capabilities.");
        tsd_demo_result_t result = run_workload_for_duration(g_tsd_config.demo_duration_sec,
                                                             g_tsd_config.work_iters);
        log_demo_result(&result);
        return;
    }

    tsd_perf_mode_t mode = tsd_perf_get_mode(perf);
    if (mode == TSD_PERF_MODE_HARDWARE) {
        tsd_perf_enable(perf);
        tsd_perf_measure_baseline(perf, &g_tsd_config);
        tsd_log_info(LOG_COMPONENT, "Perf target CPU: %d (monitor thread on CPU %d)",
                     tsd_perf_get_pinned_cpu(perf), tsd_perf_get_monitor_cpu(perf));
    } else {
        tsd_log_warn(LOG_COMPONENT, "Hardware performance counters unavailable; using software telemetry fallback.");
        tsd_perf_measure_baseline(perf, &g_tsd_config);
    }

    pthread_t monitor;
    int monitor_err = pthread_create(&monitor, NULL, thermal_monitor_thread, perf);
    if (monitor_err != 0) {
        char errbuf[128];
        tsd_log_error(LOG_COMPONENT, "Failed to start monitor thread: %s",
                      tsd_log_strerror(monitor_err, errbuf, sizeof(errbuf)));
        atomic_store_explicit(&g_tsd_running, 0, memory_order_release);
        tsd_perf_cleanup(perf);
        return;
    }

    tsd_log_info(LOG_COMPONENT, "Running workload for %d wall-clock seconds (batch iterations: %d)",
                 g_tsd_config.demo_duration_sec, g_tsd_config.work_iters);
    tsd_log_info(LOG_COMPONENT, "Tip: stress-ng --cpu 8 --cpu-load 100 to simulate thermal load");
    tsd_demo_result_t result = run_workload_for_duration(g_tsd_config.demo_duration_sec,
                                                         g_tsd_config.work_iters);
    log_demo_result(&result);
    atomic_store_explicit(&g_tsd_running, 0, memory_order_release);
    pthread_join(monitor, NULL);
    tsd_perf_cleanup(perf);
}

#undef TSD_MAYBE_UNUSED

int tsd_dispatcher_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    atomic_store_explicit(&g_tsd_last_patch_attempt, (unsigned char)SIMD_SSE41, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_last_patched_width, (unsigned char)SIMD_SSE41, memory_order_relaxed);

    int metrics_started = 0;
    if (!tsd_runtime_flags_sandbox_only() && g_tsd_config.metrics_enabled && g_tsd_config.metrics_port > 0) {
        tsd_metrics_exporter_config_t exporter_cfg = {0};
        exporter_cfg.bind_address = g_tsd_config.metrics_bind_host;
        exporter_cfg.port = (uint16_t)g_tsd_config.metrics_port;

        tsd_metrics_tls_config_t tls_cfg = {0};
        if (g_tsd_config.metrics_tls_cert_path[0] != '\0' && g_tsd_config.metrics_tls_key_path[0] != '\0') {
            tls_cfg.certificate_path = g_tsd_config.metrics_tls_cert_path;
            tls_cfg.private_key_path = g_tsd_config.metrics_tls_key_path;
            if (g_tsd_config.metrics_tls_ca_path[0] != '\0') {
                tls_cfg.ca_certificate_path = g_tsd_config.metrics_tls_ca_path;
            }
            tls_cfg.require_client_auth = g_tsd_config.metrics_tls_require_client_auth;
            exporter_cfg.tls = &tls_cfg;
        }

        tsd_metrics_basic_auth_t auth_cfg = {0};
        if (g_tsd_config.metrics_basic_auth_user[0] != '\0' &&
            g_tsd_config.metrics_basic_auth_pass[0] != '\0') {
            auth_cfg.username = g_tsd_config.metrics_basic_auth_user;
            auth_cfg.password = g_tsd_config.metrics_basic_auth_pass;
            exporter_cfg.basic_auth = &auth_cfg;
        }

        if (g_tsd_config.statsd_host[0] != '\0' && g_tsd_config.statsd_port > 0) {
            exporter_cfg.statsd_host = g_tsd_config.statsd_host;
            exporter_cfg.statsd_port = (uint16_t)g_tsd_config.statsd_port;
        }

        if (tsd_metrics_exporter_start_with_config(&exporter_cfg) == 0) {
            metrics_started = 1;
            uint16_t actual_port = tsd_metrics_exporter_listen_port();
            tsd_log_info(LOG_COMPONENT,
                         "Metrics exporter listening on %s:%u",
                         g_tsd_config.metrics_bind_host,
                         (unsigned)actual_port);
        } else {
            tsd_log_warn(LOG_COMPONENT,
                         "Failed to start metrics exporter on %s:%d",
                         g_tsd_config.metrics_bind_host,
                         g_tsd_config.metrics_port);
        }
    }

    if (tsd_trampoline_init() != 0) {
        tsd_log_error(LOG_COMPONENT, "Failed to create trampolines");
        if (metrics_started) {
            tsd_metrics_exporter_stop();
        }
        return 1;
    }

    simd_width_t max_width = tsd_detect_max_simd(&g_tsd_config);
    if (max_width == SIMD_SSE41 && !tsd_cpu_has_sse41()) {
        tsd_log_error(LOG_COMPONENT, "SSE4.1 required but not available");
        if (metrics_started) {
            tsd_metrics_exporter_stop();
        }
        return 1;
    }

    if (tsd_trampoline_patch(SIMD_SSE41) != 0) {
        tsd_log_error(LOG_COMPONENT, "Failed to install baseline trampoline patch");
        if (metrics_started) {
            tsd_metrics_exporter_stop();
        }
        return 1;
    }

    char sandbox_diag[256] = {0};
    int sandbox_rc = tsd_sandbox_run(sandbox_diag, sizeof(sandbox_diag));
    if (sandbox_rc == 0) {
        tsd_runtime_flags_record_sandbox_success();
    } else {
        tsd_runtime_flags_record_sandbox_failure(sandbox_diag);
        tsd_log_warn(LOG_COMPONENT,
                     "startup sandbox failed: %s; SIMD transitions locked to SSE4.1",
                     tsd_runtime_flags_status_message());
    }

    if (tsd_runtime_flags_sandbox_only()) {
        if (metrics_started) {
            tsd_metrics_exporter_stop();
        }
        return sandbox_rc == 0 ? 0 : 1;
    }

    if (tsd_runtime_flags_allow_transitions() && max_width != SIMD_SSE41) {
        if (tsd_trampoline_patch(max_width) != 0) {
            tsd_log_error(LOG_COMPONENT, "Failed to install initial trampoline patch");
            if (metrics_started) {
                tsd_metrics_exporter_stop();
            }
            return 1;
        }
    } else if (!tsd_runtime_flags_allow_transitions() && max_width != SIMD_SSE41) {
        tsd_log_warn(LOG_COMPONENT,
                     "Operating in safe SIMD mode; detected maximum %d but constrained: %s",
                     (int)max_width,
                     tsd_runtime_flags_status_message());
    }

    print_configuration(max_width);

    if (g_tsd_config.health_check_mode) {
        tsd_log_info(LOG_COMPONENT, "Running health check mode");
        int rc = tsd_run_health_check();
        if (metrics_started) {
            tsd_metrics_exporter_stop();
        }
        return (sandbox_rc == 0) ? rc : 1;
    }

    struct sigaction reload_action;
    memset(&reload_action, 0, sizeof(reload_action));
    reload_action.sa_handler = tsd_handle_sighup;
    sigemptyset(&reload_action.sa_mask);
    if (sigaction(SIGHUP, &reload_action, NULL) != 0) {
        char errbuf[128];
        tsd_log_warn(LOG_COMPONENT, "Unable to install executable SIGHUP reload handler: %s",
                     tsd_log_strerror(errno, errbuf, sizeof(errbuf)));
    }

    run_demo();
    if (metrics_started) {
        tsd_metrics_exporter_stop();
    }
    return 0;
}

#ifdef TSD_ENABLE_TESTS
static void tsd_reset_patch_state(void) {
    atomic_store_explicit(&g_tsd_current_width, SIMD_SSE41, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_current_width_byte, (unsigned char)SIMD_SSE41, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_trampoline_initialized, 0, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_active_trampoline, g_tsd_trampoline_ctx.active, memory_order_seq_cst);
    atomic_store_explicit(&g_tsd_last_patch_attempt, (unsigned char)SIMD_SSE41, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_last_patched_width, (unsigned char)SIMD_SSE41, memory_order_relaxed);
}

void tsd_test_reset_runtime(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    tsd_trampoline_clear_overrides();
    tsd_perf_clear_fake_script();
    tsd_cpu_clear_detect_override();
    tsd_reset_patch_state();
    atomic_store_explicit(&g_tsd_running, 1, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_reload_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_workload_iterations, 0, memory_order_relaxed);
    tsd_runtime_config_refresh_ticks(&g_tsd_config);
    tsd_runtime_flags_record_sandbox_success();
}

void tsd_test_set_policy_counts(int down, int up) {
    if (down > 0) {
        g_tsd_config.down_count = down;
    }
    if (up > 0) {
        g_tsd_config.up_count = up;
    }
}

void tsd_test_set_timing(int interval_us, int cooldown_down_ms, int cooldown_up_ms, int dwell_ms) {
    if (interval_us > 0) {
        g_tsd_config.check_interval_us = interval_us;
    }
    if (cooldown_down_ms >= 0) {
        g_tsd_config.cooldown_down_ms = cooldown_down_ms;
    }
    if (cooldown_up_ms >= 0) {
        g_tsd_config.cooldown_up_ms = cooldown_up_ms;
    }
    if (dwell_ms >= 0) {
        g_tsd_config.min_dwell_ms = dwell_ms;
    }
}

int tsd_test_refresh_ticks(void) {
    return tsd_runtime_config_refresh_ticks(&g_tsd_config);
}

void tsd_test_set_running(int value) {
    atomic_store_explicit(&g_tsd_running, value, memory_order_release);
}

void tsd_test_run_workload(int iterations) {
    if (iterations < 0) {
        return;
    }
    workload_loop(iterations);
}

void tsd_test_reset_workload_counter(void) {
    atomic_store_explicit(&g_tsd_workload_iterations, 0, memory_order_relaxed);
}

void tsd_test_set_detect_override(simd_width_t (*fn)(void)) {
    tsd_cpu_set_detect_override(fn);
}

void tsd_test_clear_detect_override(void) {
    tsd_cpu_clear_detect_override();
}

void tsd_test_override_patch(simd_width_t width, const uint8_t *bytes, size_t len) {
    tsd_trampoline_override_patch(width, bytes, len);
}

void tsd_test_clear_patch_overrides(void) {
    tsd_trampoline_clear_overrides();
}

const uint8_t* tsd_test_patch_bytes(simd_width_t width, size_t *len) {
    return tsd_trampoline_patch_bytes(width, len);
}

void tsd_test_set_fake_perf_script(const uint32_t *ratios, size_t count, uint32_t mpki) {
    tsd_perf_set_fake_script(ratios, count, mpki);
}

void tsd_test_set_fake_telemetry(const tsd_telemetry_sample_t *samples, size_t count) {
    tsd_perf_set_fake_telemetry(samples, count);
}

void tsd_test_clear_fake_perf_script(void) {
    tsd_perf_clear_fake_script();
}

const char* tsd_test_last_patch_error(void) {
    return tsd_trampoline_last_error();
}

void tsd_test_force_patch_failure(tsd_patch_fail_stage_t stage) {
    tsd_trampoline_force_failure(stage);
}

simd_width_t tsd_test_current_width(void) {
    return atomic_load_explicit(&g_tsd_current_width, memory_order_relaxed);
}

unsigned char tsd_test_last_patched_width(void) {
    return atomic_load_explicit(&g_tsd_last_patched_width, memory_order_relaxed);
}

simd_width_t tsd_test_detect_host_max(void) {
    return tsd_cpu_detect_ignoring_override(&g_tsd_config);
}

int tsd_test_patch(simd_width_t width) {
    return tsd_trampoline_patch(width);
}

int tsd_test_inactive_page_writable(void) {
    return tsd_trampoline_inactive_page_writable();
}

perf_ctx_t* tsd_test_init_perf(void) {
    return tsd_perf_init(workload_once);
}

void tsd_test_measure_baseline(perf_ctx_t *ctx) {
    tsd_perf_measure_baseline(ctx, &g_tsd_config);
}

void tsd_test_cleanup_perf(perf_ctx_t *ctx) {
    tsd_perf_cleanup(ctx);
}

perf_ctx_t* tsd_test_perf_create_dummy_context(void) {
    return tsd_perf_test_create_dummy_context();
}

void tsd_test_perf_destroy_dummy_context(perf_ctx_t *ctx) {
    tsd_perf_test_destroy_dummy_context(ctx);
}

void tsd_test_perf_set_group_fd(perf_ctx_t *ctx, int fd) {
    tsd_perf_test_set_group_fd(ctx, fd);
}

void tsd_test_perf_set_llc_fd(perf_ctx_t *ctx, int fd) {
    tsd_perf_test_set_llc_fd(ctx, fd);
}

void tsd_test_perf_set_mode(perf_ctx_t *ctx, tsd_perf_mode_t mode) {
    tsd_perf_test_set_mode(ctx, mode);
}

void tsd_test_perf_set_read_streams(const tsd_perf_test_read_stream_t *streams, size_t count) {
    tsd_perf_test_set_read_streams(streams, count);
}

void tsd_test_perf_clear_read_streams(void) {
    tsd_perf_test_clear_read_streams();
}

uint64_t tsd_test_perf_get_baseline_cpi(const perf_ctx_t *ctx) {
    return tsd_perf_test_get_baseline_cpi(ctx);
}

uint64_t tsd_test_perf_get_baseline_mpki(const perf_ctx_t *ctx) {
    return tsd_perf_test_get_baseline_mpki(ctx);
}

int tsd_test_perf_get_last_group_valid(const perf_ctx_t *ctx) {
    return tsd_perf_test_get_last_group_valid(ctx);
}

uint64_t tsd_test_perf_get_last_llc_value(const perf_ctx_t *ctx) {
    return tsd_perf_test_get_last_llc_value(ctx);
}

#endif