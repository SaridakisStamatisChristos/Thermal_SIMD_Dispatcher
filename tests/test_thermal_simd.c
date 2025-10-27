#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "thermal_simd_test.h"

static simd_width_t force_avx2(void) {
    return SIMD_AVX2;
}

static int wait_for_width(simd_width_t expected, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        if (tsd_test_current_width() == expected) {
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

static int run_monitor_thread_scenario(void) {
    setenv("TSD_FAKE_PERF", "1", 1);
    tsd_test_reset_runtime();
    tsd_test_set_policy_counts(1, 1);
    tsd_test_set_timing(1000, 1, 1, 1);
    if (tsd_test_refresh_ticks() != 0) {
        fprintf(stderr, "failed to refresh ticks\n");
        return 1;
    }
    tsd_test_set_detect_override(force_avx2);
    size_t patch_len = 0;
    const uint8_t *sse_patch = tsd_test_patch_bytes(SIMD_SSE41, &patch_len);
    tsd_test_override_patch(SIMD_AVX2, sse_patch, patch_len);
    tsd_test_override_patch(SIMD_AVX512, sse_patch, patch_len);
    if (init_double_buffer_trampoline() != 0) {
        fprintf(stderr, "failed to init trampoline\n");
        return 1;
    }
    tsd_test_patch(SIMD_SSE41);
    perf_ctx_t *ctx = tsd_test_init_perf();
    if (!ctx) {
        fprintf(stderr, "failed to init perf context\n");
        return 1;
    }
    tsd_test_measure_baseline(ctx);
    tsd_test_set_fake_perf_script((const uint32_t[]){2100, 900, 900, 900, 900, 900}, 6, 0);
    tsd_test_patch(SIMD_AVX2);
    tsd_test_set_running(1);
    pthread_t thread;
    if (pthread_create(&thread, NULL, thermal_monitor_thread, ctx) != 0) {
        fprintf(stderr, "failed to start monitor thread\n");
        tsd_test_cleanup_perf(ctx);
        return 1;
    }
    int rc = 0;
    if (wait_for_width(SIMD_SSE41, 500) != 0) {
        fprintf(stderr, "width did not downgrade\n");
        rc = 1;
    }
    if (rc == 0 && wait_for_width(SIMD_AVX2, 500) != 0) {
        fprintf(stderr, "width did not upgrade\n");
        rc = 1;
    }
    tsd_test_set_running(0);
    pthread_join(thread, NULL);
    tsd_test_cleanup_perf(ctx);
    tsd_test_clear_detect_override();
    tsd_test_clear_fake_perf_script();
    tsd_test_clear_patch_overrides();
    unsetenv("TSD_FAKE_PERF");
    return rc;
}

static int run_patch_failure_diagnostic(void) {
    tsd_test_reset_runtime();
    size_t patch_len = 0;
    const uint8_t *sse_patch = tsd_test_patch_bytes(SIMD_SSE41, &patch_len);
    tsd_test_override_patch(SIMD_AVX2, sse_patch, patch_len);
    tsd_test_force_patch_failure(TSD_PATCH_FAIL_PROTECT_WRITE);
    tsd_test_patch(SIMD_AVX2);
    const char *err = tsd_test_last_patch_error();
    if (!err || strstr(err, "mprotect(trampoline write)") == NULL) {
        fprintf(stderr, "expected patch error message, got '%s'\n", err ? err : "<null>");
        return 1;
    }
    if (tsd_test_current_width() != SIMD_SSE41) {
        fprintf(stderr, "width changed unexpectedly after failed patch\n");
        return 1;
    }
    tsd_test_clear_patch_overrides();
    return 0;
}

int main(void) {
    tsd_test_reset_runtime();
    if (init_double_buffer_trampoline() != 0) {
        fprintf(stderr, "initial trampoline init failed\n");
        return 1;
    }
    if (run_monitor_thread_scenario() != 0) {
        return 1;
    }
    if (run_patch_failure_diagnostic() != 0) {
        return 1;
    }
    return 0;
}
