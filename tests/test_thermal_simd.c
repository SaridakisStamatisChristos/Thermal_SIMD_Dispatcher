#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
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
    int rc = 0;
    int monitor_started = 0;
    pthread_t thread = {0};
    perf_ctx_t *ctx = NULL;

    setenv("TSD_FAKE_PERF", "1", 1);
    tsd_test_reset_runtime();
    tsd_test_set_policy_counts(1, 1);
    tsd_test_set_timing(1000, 1, 1, 1);
    if (tsd_test_refresh_ticks() != 0) {
        fprintf(stderr, "failed to refresh ticks\n");
        rc = 1;
        goto out;
    }
    tsd_test_set_detect_override(force_avx2);
    size_t patch_len = 0;
    const uint8_t *sse_patch = tsd_test_patch_bytes(SIMD_SSE41, &patch_len);
    tsd_test_override_patch(SIMD_AVX2, sse_patch, patch_len);
    tsd_test_override_patch(SIMD_AVX512, sse_patch, patch_len);
    if (init_double_buffer_trampoline() != 0) {
        fprintf(stderr, "failed to init trampoline\n");
        rc = 1;
        goto out;
    }
    if (tsd_test_patch(SIMD_SSE41) != 0) {
        fprintf(stderr, "failed to patch SSE4.1 baseline\n");
        rc = 1;
        goto out;
    }
    ctx = tsd_test_init_perf();
    if (!ctx) {
        fprintf(stderr, "failed to init perf context\n");
        rc = 1;
        goto out;
    }
    tsd_test_measure_baseline(ctx);
    tsd_test_set_fake_perf_script((const uint32_t[]){2100, 900, 900, 900, 900, 900}, 6, 0);
    if (tsd_test_patch(SIMD_AVX2) != 0) {
        fprintf(stderr, "failed to patch AVX2 baseline\n");
        rc = 1;
        goto out;
    }
    tsd_test_set_running(1);
    if (pthread_create(&thread, NULL, thermal_monitor_thread, ctx) != 0) {
        fprintf(stderr, "failed to start monitor thread\n");
        rc = 1;
        goto out;
    }
    monitor_started = 1;
    if (wait_for_width(SIMD_SSE41, 500) != 0) {
        fprintf(stderr, "width did not downgrade\n");
        rc = 1;
    }
    if (rc == 0 && wait_for_width(SIMD_AVX2, 500) != 0) {
        fprintf(stderr, "width did not upgrade\n");
        rc = 1;
    }
    if (rc == 0) {
        tsd_test_force_patch_failure(TSD_PATCH_FAIL_PROTECT_WRITE);
        tsd_test_set_fake_perf_script((const uint32_t[]){2100, 900, 900, 900}, 4, 0);
        usleep(20000);
        if (tsd_test_current_width() != SIMD_AVX2) {
            fprintf(stderr, "width changed unexpectedly after forced failure\n");
            rc = 1;
        }
        tsd_test_force_patch_failure(TSD_PATCH_FAIL_NONE);
    }
    if (rc == 0) {
        tsd_test_set_fake_perf_script((const uint32_t[]){2100, 2100, 2100, 900, 900}, 5, 0);
        if (wait_for_width(SIMD_SSE41, 1000) != 0) {
            fprintf(stderr, "width did not downgrade after failure cleared\n");
            rc = 1;
        }
    }
    if (rc == 0) {
        tsd_test_set_fake_perf_script((const uint32_t[]){900, 900, 900}, 3, 0);
        if (wait_for_width(SIMD_AVX2, 500) != 0) {
            fprintf(stderr, "width did not upgrade after retry\n");
            rc = 1;
        }
    }

out:
    tsd_test_set_running(0);
    if (monitor_started) {
        pthread_join(thread, NULL);
    }
    if (ctx) {
        tsd_test_cleanup_perf(ctx);
    }
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
    if (tsd_test_patch(SIMD_AVX2) == 0) {
        fprintf(stderr, "patch unexpectedly succeeded under forced failure\n");
        return 1;
    }
    const char *err = tsd_test_last_patch_error();
    if (!err || strstr(err, "mprotect(trampoline write)") == NULL) {
        fprintf(stderr, "expected patch error message, got '%s'\n", err ? err : "<null>");
        return 1;
    }
    if (tsd_test_current_width() != SIMD_SSE41) {
        fprintf(stderr, "width changed unexpectedly after failed patch\n");
        return 1;
    }
    tsd_test_force_patch_failure(TSD_PATCH_FAIL_PROTECT_EXEC);
    if (tsd_test_patch(SIMD_AVX2) == 0) {
        fprintf(stderr, "patch unexpectedly succeeded under exec protection failure\n");
        return 1;
    }
    err = tsd_test_last_patch_error();
    if (!err || strstr(err, "mprotect(trampoline exec)") == NULL) {
        fprintf(stderr, "expected exec patch error message, got '%s'\n", err ? err : "<null>");
        return 1;
    }
    int inactive_writable = tsd_test_inactive_page_writable();
    if (inactive_writable != 0) {
        fprintf(stderr, "inactive page left writable after failure\n");
        return 1;
    }
    tsd_test_force_patch_failure(TSD_PATCH_FAIL_NONE);
    tsd_test_clear_patch_overrides();
    return 0;
}

static int run_perf_read_retry_test(void) {
    typedef struct {
        uint64_t nr;
        uint64_t time_enabled;
        uint64_t time_running;
        uint64_t values[2];
    } perf_group_read_test_t;

    int rc = 0;
    perf_ctx_t *ctx = tsd_test_perf_create_dummy_context();
    if (!ctx) {
        fprintf(stderr, "failed to create dummy perf context\n");
        return 1;
    }
    tsd_test_perf_set_mode(ctx, TSD_PERF_MODE_HARDWARE);
    tsd_test_perf_set_group_fd(ctx, 100);
    tsd_test_perf_set_llc_fd(ctx, 101);

    const perf_group_read_test_t rd_before = {
        .nr = 2,
        .time_enabled = 100,
        .time_running = 100,
        .values = {1000, 500},
    };
    const perf_group_read_test_t rd_after = {
        .nr = 2,
        .time_enabled = 200,
        .time_running = 200,
        .values = {3000, 2500},
    };
    uint8_t group_bytes[sizeof(rd_before) + sizeof(rd_after)] = {0};
    memcpy(group_bytes, &rd_before, sizeof(rd_before));
    memcpy(group_bytes + sizeof(rd_before), &rd_after, sizeof(rd_after));

    const uint64_t llc_values[2] = {10, 30};

    const tsd_perf_test_read_step_t group_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 16},
        {TSD_PERF_TEST_STEP_DATA, sizeof(rd_before) - 16},
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 8},
        {TSD_PERF_TEST_STEP_DATA, sizeof(rd_after) - 8},
    };
    const tsd_perf_test_read_step_t llc_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 5},
        {TSD_PERF_TEST_STEP_DATA, 3},
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 2},
        {TSD_PERF_TEST_STEP_DATA, 6},
    };
    const tsd_perf_test_read_stream_t baseline_streams[] = {
        {100, group_steps, sizeof(group_steps) / sizeof(group_steps[0]), group_bytes, sizeof(group_bytes)},
        {101, llc_steps, sizeof(llc_steps) / sizeof(llc_steps[0]), (const uint8_t *)llc_values, sizeof(llc_values)},
    };
    tsd_test_perf_set_read_streams(baseline_streams, sizeof(baseline_streams) / sizeof(baseline_streams[0]));
    tsd_perf_measure_baseline(ctx, NULL);
    tsd_test_perf_clear_read_streams();

    if (tsd_test_perf_get_baseline_cpi(ctx) != 1000 ||
        tsd_test_perf_get_baseline_mpki(ctx) == 0 ||
        !tsd_test_perf_get_last_group_valid(ctx) ||
        tsd_test_perf_get_last_llc_value(ctx) != llc_values[1]) {
        fprintf(stderr, "baseline read retry verification failed\n");
        rc = 1;
        goto out;
    }

    const perf_group_read_test_t rd_next = {
        .nr = 2,
        .time_enabled = 300,
        .time_running = 300,
        .values = {7000, 4500},
    };
    const tsd_perf_test_read_step_t eval_group_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 12},
        {TSD_PERF_TEST_STEP_DATA, sizeof(rd_next) - 12},
    };
    const uint64_t llc_next = 60;
    const tsd_perf_test_read_step_t eval_llc_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 5},
        {TSD_PERF_TEST_STEP_DATA, 3},
    };
    uint8_t eval_group_bytes[sizeof(rd_next)] = {0};
    memcpy(eval_group_bytes, &rd_next, sizeof(rd_next));
    const tsd_perf_test_read_stream_t eval_streams[] = {
        {100, eval_group_steps, sizeof(eval_group_steps) / sizeof(eval_group_steps[0]), eval_group_bytes, sizeof(eval_group_bytes)},
        {101, eval_llc_steps, sizeof(eval_llc_steps) / sizeof(eval_llc_steps[0]), (const uint8_t *)&llc_next, sizeof(llc_next)},
    };
    tsd_test_perf_set_read_streams(eval_streams, sizeof(eval_streams) / sizeof(eval_streams[0]));
    tsd_thermal_eval_t eval = {0};
    int triggered = tsd_perf_evaluate(ctx, &eval, NULL);
    tsd_test_perf_clear_read_streams();

    if (!tsd_test_perf_get_last_group_valid(ctx) ||
        tsd_test_perf_get_last_llc_value(ctx) != llc_next ||
        eval.cpi_milli == 0 ||
        triggered == 0) {
        fprintf(stderr, "evaluation read retry verification failed\n");
        rc = 1;
    }

out:
    tsd_test_perf_destroy_dummy_context(ctx);
    return rc;
}

int main(void) {
    tsd_test_reset_runtime();
    if (init_double_buffer_trampoline() != 0) {
        fprintf(stderr, "initial trampoline init failed\n");
        return 1;
    }
    if (run_perf_read_retry_test() != 0) {
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
