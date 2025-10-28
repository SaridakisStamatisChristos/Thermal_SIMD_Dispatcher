#define _GNU_SOURCE
#include "stress_common.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

pthread_mutex_t tsd_stress_patch_mutex = PTHREAD_MUTEX_INITIALIZER;

static void tsd_stress_setenv(const char *key, const char *value) {
    if (setenv(key, value, 1) != 0) {
        fprintf(stderr, "failed to set %s=%s: %s\n", key, value, strerror(errno));
    }
}

int tsd_stress_prepare_runtime(void) {
    tsd_stress_setenv("TSD_FAKE_PERF", "1");
    tsd_stress_setenv("TSD_ALLOW_SOFTWARE_UPGRADES", "1");
    tsd_test_reset_runtime();
    tsd_test_reset_workload_counter();
    tsd_test_set_policy_counts(1, 1);
    tsd_test_set_timing(5000, 5, 5, 5);
    if (tsd_test_refresh_ticks() != 0) {
        fprintf(stderr, "failed to refresh tick cache\n");
        return -1;
    }
    if (init_double_buffer_trampoline() != 0) {
        fprintf(stderr, "failed to initialize trampoline\n");
        return -1;
    }
    size_t patch_len = 0;
    const uint8_t *sse_patch = tsd_test_patch_bytes(SIMD_SSE41, &patch_len);
    if (!sse_patch || patch_len == 0) {
        fprintf(stderr, "failed to fetch baseline patch bytes\n");
        return -1;
    }
    tsd_test_override_patch(SIMD_AVX2, sse_patch, patch_len);
    tsd_test_override_patch(SIMD_AVX512, sse_patch, patch_len);
    if (tsd_test_patch(SIMD_SSE41) != 0) {
        fprintf(stderr, "failed to seed SSE4.1 trampoline\n");
        return -1;
    }
    return 0;
}

static void tsd_stress_clear_env(const char *key) {
    if (unsetenv(key) != 0) {
        fprintf(stderr, "failed to unset %s: %s\n", key, strerror(errno));
    }
}

void tsd_stress_teardown_runtime(void) {
    pthread_mutex_lock(&tsd_stress_patch_mutex);
    tsd_test_force_patch_failure(TSD_PATCH_FAIL_NONE);
    pthread_mutex_unlock(&tsd_stress_patch_mutex);
    tsd_test_clear_patch_overrides();
    tsd_test_clear_fake_perf_script();
    tsd_test_set_fake_telemetry(NULL, 0);
    tsd_test_clear_detect_override();
    tsd_stress_clear_env("TSD_FAKE_PERF");
    tsd_stress_clear_env("TSD_ALLOW_SOFTWARE_UPGRADES");
}

int tsd_stress_patch(simd_width_t width) {
    int rc = 0;
    pthread_mutex_lock(&tsd_stress_patch_mutex);
    rc = tsd_test_patch(width);
    pthread_mutex_unlock(&tsd_stress_patch_mutex);
    return rc;
}

int tsd_stress_inject_failure(tsd_patch_fail_stage_t stage, simd_width_t width) {
    int rc = 0;
    pthread_mutex_lock(&tsd_stress_patch_mutex);
    tsd_test_force_patch_failure(stage);
    rc = tsd_test_patch(width);
    tsd_test_force_patch_failure(TSD_PATCH_FAIL_NONE);
    pthread_mutex_unlock(&tsd_stress_patch_mutex);
    return rc;
}
