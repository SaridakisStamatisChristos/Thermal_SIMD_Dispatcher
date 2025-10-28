#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_cpu.h>
#include <thermal/simd/thermal_perf.h>
#include <thermal/simd/thermal_signals.h>
#include <thermal/simd/thermal_trampoline.h>

#ifdef TSD_ENABLE_TESTS
#include "thermal_simd_test.h"
#endif

static _Atomic int g_tsd_running = 1;

static int32_t simd_shim(int32_t a, int32_t b);

static inline void workload_once(void) {
    (void)simd_shim(42, 7);
    atomic_fetch_add_explicit(&g_tsd_workload_iterations, 1, memory_order_relaxed);
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

static void print_configuration(simd_width_t max_width) {
    printf("Maximum supported: %s%s\n",
           max_width == SIMD_AVX512 ? "AVX-512 (XMM-only)" :
           max_width == SIMD_AVX2 ? "AVX2 (XMM-only)" : "SSE4.1",
           g_tsd_config.allow_avx512 ? "" : " [AVX-512 disabled by policy]");
    printf("AVX transition guard: %s\n", g_tsd_avx_available ? "enabled" : "disabled");
    printf("Configuration:\n");
    printf("  Check interval: %d ms\n", g_tsd_config.check_interval_us / 1000);
    printf("  Down threshold: %.1fx CPI (after %d events)\n",
           g_tsd_config.down_ratio, g_tsd_config.down_count);
    printf("  Up threshold: %d stable events\n", g_tsd_config.up_count);
    printf("  Cooldown: %d ms down, %d ms up\n",
           g_tsd_config.cooldown_down_ms, g_tsd_config.cooldown_up_ms);
    printf("  Minimum dwell: %d ms per width\n", g_tsd_config.min_dwell_ms);
    printf("  Memory guard: divisor=%d offset=%d milli\n",
           g_tsd_config.memory_guard_divisor, g_tsd_config.memory_guard_offset_milli);
    printf("  Cooldown ticks: down=%d up=%d min-dwell=%d\n",
           g_tsd_config.cooldown_down_ticks,
           g_tsd_config.cooldown_up_ticks,
           g_tsd_config.min_dwell_ticks);
    printf("  Demo: %d sec, work iters: %d\n\n",
           g_tsd_config.demo_duration_sec, g_tsd_config.work_iters);
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
        if (cooldown > 0) {
            cooldown--;
            continue;
        }
        if (dwell_ticks < g_tsd_config.min_dwell_ticks) {
            continue;
        }
        tsd_thermal_eval_t eval = {0};
        if (evaluate_thermal(ctx, &eval)) {
            throttle_count++;
            stable_count = 0;
            if (throttle_count >= g_tsd_config.down_count && width > SIMD_SSE41) {
                printf("\nThermal throttle: ratio=%u.%03u (trimmed %u.%03u) severity=+%lu.%03lu MPKI=%lu.%03lu%s\n",
                       eval.ratio_milli / 1000, eval.ratio_milli % 1000,
                       eval.trimmed_ratio_milli / 1000, eval.trimmed_ratio_milli % 1000,
                       eval.severity_milli / 1000, eval.severity_milli % 1000,
                       eval.llc_mpki_milli / 1000, eval.llc_mpki_milli % 1000,
                       eval.memory_bound ? " [memory bound guard raised]" : "");
                simd_width_t target = width - 1;
                int patch_rc = tsd_trampoline_patch(target);
                if (patch_rc == 0) {
                    width = target;
                } else {
                    int patch_err = errno;
                    width = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
#ifdef TSD_ENABLE_TESTS
                    const char *patch_err_msg = tsd_trampoline_last_error();
                    if (patch_err_msg && patch_err_msg[0] != '\0') {
                        fprintf(stderr, "[thermal_simd] downgrade patch failed: %s\n", patch_err_msg);
                    } else
#endif
                    if (patch_err != 0) {
                        fprintf(stderr, "[thermal_simd] downgrade patch failed: %s\n", strerror(patch_err));
                    } else {
                        fprintf(stderr, "[thermal_simd] downgrade patch failed\n");
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
                printf("\nRecovered: ratio=%u.%03u (trimmed %u.%03u) MPKI=%lu.%03lu\n",
                       eval.ratio_milli / 1000, eval.ratio_milli % 1000,
                       eval.trimmed_ratio_milli / 1000, eval.trimmed_ratio_milli % 1000,
                       eval.llc_mpki_milli / 1000, eval.llc_mpki_milli % 1000);
                simd_width_t target = width + 1;
                int patch_rc = tsd_trampoline_patch(target);
                if (patch_rc == 0) {
                    width = target;
                } else {
                    int patch_err = errno;
                    width = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
#ifdef TSD_ENABLE_TESTS
                    const char *patch_err_msg = tsd_trampoline_last_error();
                    if (patch_err_msg && patch_err_msg[0] != '\0') {
                        fprintf(stderr, "[thermal_simd] upgrade patch failed: %s\n", patch_err_msg);
                    } else
#endif
                    if (patch_err != 0) {
                        fprintf(stderr, "[thermal_simd] upgrade patch failed: %s\n", strerror(patch_err));
                    } else {
                        fprintf(stderr, "[thermal_simd] upgrade patch failed\n");
                    }
                }
                stable_count = 0;
                cooldown = g_tsd_config.cooldown_up_ticks;
                dwell_ticks = 0;
            }
        }
    }
    return NULL;
}

static void run_demo(void) {
    perf_ctx_t *perf = tsd_perf_init(workload_once);
    if (!perf) {
        printf("\nPerformance monitoring unavailable.\n");
        printf("Suggestions:\n");
        printf("  - Run as root: sudo ./thermal_simd\n");
        printf("  - Or: sudo sysctl kernel.perf_event_paranoid=0\n");
        printf("  - Container: add --cap-add=SYS_ADMIN or --privileged\n\n");
        printf("Running without thermal adaptation...\n");
        workload_loop(100000000);
        return;
    }

    tsd_perf_mode_t mode = tsd_perf_get_mode(perf);
    if (mode == TSD_PERF_MODE_HARDWARE) {
        tsd_perf_enable(perf);
        tsd_perf_measure_baseline(perf, &g_tsd_config);
        printf("Perf target CPU: %d (monitor thread on CPU %d)\n",
               tsd_perf_get_pinned_cpu(perf), tsd_perf_get_monitor_cpu(perf));
    } else {
        printf("\nHardware performance counters unavailable; using software telemetry fallback.\n");
        tsd_perf_measure_baseline(perf, &g_tsd_config);
    }

    pthread_t monitor;
    int monitor_err = pthread_create(&monitor, NULL, thermal_monitor_thread, perf);
    if (monitor_err != 0) {
        fprintf(stderr, "ERROR: Failed to start monitor thread: %s\n", strerror(monitor_err));
        atomic_store_explicit(&g_tsd_running, 0, memory_order_release);
        tsd_perf_cleanup(perf);
        return;
    }

    printf("\nRunning workload for %d seconds...\n", g_tsd_config.demo_duration_sec);
    printf("Try: stress-ng --cpu 8 --cpu-load 100  (to simulate thermal load)\n\n");
    for (int sec = 0; sec < g_tsd_config.demo_duration_sec; sec++) {
        workload_loop(g_tsd_config.work_iters);
        printf(".");
        fflush(stdout);
    }
    printf("\n\nDone.\n");
    atomic_store_explicit(&g_tsd_running, 0, memory_order_release);
    pthread_join(monitor, NULL);
    tsd_perf_cleanup(perf);
}

#ifndef TSD_ENABLE_TESTS
int main(int argc, char **argv) {
    printf("=== Production Thermal-Aware SIMD Dispatcher ===\n\n");
    tsd_runtime_config_parse_cli(&g_tsd_config, argc, argv);
    tsd_install_patch_signal_handlers();
    atomic_store_explicit(&g_tsd_last_patch_attempt, (unsigned char)SIMD_SSE41, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_last_patched_width, (unsigned char)SIMD_SSE41, memory_order_relaxed);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);

    if (tsd_trampoline_init() != 0) {
        fprintf(stderr, "Failed to create trampolines\n");
        return 1;
    }

    simd_width_t max_width = tsd_detect_max_simd(&g_tsd_config);
    if (max_width == SIMD_SSE41 && !tsd_cpu_has_sse41()) {
        fprintf(stderr, "ERROR: SSE4.1 required but not available\n");
        return 1;
    }

    if (tsd_trampoline_patch(max_width) != 0) {
        fprintf(stderr, "Failed to install initial trampoline patch\n");
        return 1;
    }
    print_configuration(max_width);
    run_demo();
    return 0;
}
#endif

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
    atomic_store_explicit(&g_tsd_workload_iterations, 0, memory_order_relaxed);
    tsd_runtime_config_refresh_ticks(&g_tsd_config);
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

#endif
