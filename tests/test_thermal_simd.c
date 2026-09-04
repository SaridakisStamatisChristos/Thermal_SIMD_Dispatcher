#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <thermal/simd/telemetry_fusion.h>
#include <thermal/simd/thermal_config.h>

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
    tsd_runtime_config_exit_degraded_mode(&g_tsd_config, "tests");
    g_tsd_config.degraded_policy_active = 0;
    tsd_test_measure_baseline(ctx);
    tsd_test_perf_set_mode(ctx, TSD_PERF_MODE_HARDWARE);

    /*
     * The production runtime now treats package temperature as explicit
     * authorization for wider SIMD. Keep this monitor scenario deterministic
     * on CI hosts that expose no thermal zone by providing a valid cool sample
     * to both the policy script and the fusion safety gate.
     */
    const tsd_telemetry_sample_t cool_telemetry = {
        .temp_available = 1,
        .freq_ratio_available = 1,
        .package_temp_millic = 70000,
        .freq_ratio_milli = 1000,
    };
    tsd_test_set_fake_telemetry(&cool_telemetry, 1);
    if (tsd_telemetry_fusion_publish_sample(&cool_telemetry) != 0) {
        fprintf(stderr, "failed to publish cool telemetry authorization\n");
        rc = 1;
        goto out;
    }

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
    tsd_test_set_fake_telemetry(NULL, 0);
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
    if (!err || strstr(err, "immutable trampoline selection rejected (write fault injection)") == NULL) {
        fprintf(stderr, "expected immutable write-fault diagnostic, got '%s'\n", err ? err : "<null>");
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
    if (!err || strstr(err, "immutable trampoline selection rejected (RX fault injection)") == NULL) {
        fprintf(stderr, "expected immutable RX-fault diagnostic, got '%s'\n", err ? err : "<null>");
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

static int run_software_timeout_trip(void) {
    int rc = 0;
    perf_ctx_t *ctx = NULL;
    setenv("TSD_FAKE_PERF", "1", 1);
    tsd_test_reset_runtime();
    if (init_double_buffer_trampoline() != 0) {
        fprintf(stderr, "failed to init trampoline for timeout test\n");
        rc = 1;
        goto out;
    }
    if (tsd_test_patch(SIMD_SSE41) != 0) {
        fprintf(stderr, "failed to patch baseline for timeout test\n");
        rc = 1;
        goto out;
    }
    ctx = tsd_test_init_perf();
    if (!ctx) {
        fprintf(stderr, "failed to init perf context for timeout test\n");
        rc = 1;
        goto out;
    }
    tsd_test_perf_set_mode(ctx, TSD_PERF_MODE_SOFTWARE);
    tsd_test_perf_rewind_mode(ctx, 3600);
    if (!tsd_perf_check_software_timeout(ctx, 5)) {
        fprintf(stderr, "software timeout did not trigger\n");
        rc = 1;
        goto out;
    }
    if (tsd_perf_check_software_timeout(ctx, 5)) {
        fprintf(stderr, "software timeout retriggered unexpectedly\n");
        rc = 1;
        goto out;
    }
    tsd_test_perf_rewind_mode(ctx, 0);
    if (tsd_perf_check_software_timeout(ctx, 5)) {
        fprintf(stderr, "software timeout triggered without elapsed time\n");
        rc = 1;
    }

out:
    if (ctx) {
        tsd_test_cleanup_perf(ctx);
    }
    unsetenv("TSD_FAKE_PERF");
    return rc;
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

    /*
     * This regression is intentionally scoped to read semantics. The primary
     * perf-group RESET/ENABLE contract is validated independently by the
     * production recovery path; this synthetic context models only the group
     * leader stream and therefore must not fabricate an instruction-event FD.
     */
    const perf_group_read_test_t rd_seed = {
        .nr = 2,
        .time_enabled = 100,
        .time_running = 100,
        .values = {1000, 500},
    };
    const uint64_t llc_seed = 10;
    const tsd_perf_test_read_step_t seed_group_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 16},
        {TSD_PERF_TEST_STEP_DATA, sizeof(rd_seed) - 16},
    };
    const tsd_perf_test_read_step_t seed_llc_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 5},
        {TSD_PERF_TEST_STEP_DATA, 3},
    };
    const tsd_perf_test_read_stream_t seed_streams[] = {
        {100, seed_group_steps, sizeof(seed_group_steps) / sizeof(seed_group_steps[0]),
         (const uint8_t *)&rd_seed, sizeof(rd_seed)},
        {101, seed_llc_steps, sizeof(seed_llc_steps) / sizeof(seed_llc_steps[0]),
         (const uint8_t *)&llc_seed, sizeof(llc_seed)},
    };
    tsd_test_perf_set_read_streams(seed_streams, sizeof(seed_streams) / sizeof(seed_streams[0]));
    tsd_thermal_eval_t eval = {0};
    int triggered = tsd_perf_evaluate(ctx, &eval, NULL);
    tsd_test_perf_clear_read_streams();
    if (triggered != 0 || !tsd_test_perf_get_last_group_valid(ctx) ||
        tsd_test_perf_get_last_llc_value(ctx) != llc_seed) {
        fprintf(stderr, "initial partial group read did not seed evaluator state\n");
        rc = 1;
        goto out;
    }

    const perf_group_read_test_t rd_stable = {
        .nr = 2,
        .time_enabled = 200,
        .time_running = 200,
        .values = {3000, 2500},
    };
    const uint64_t llc_stable = 30;
    const tsd_perf_test_read_step_t stable_group_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 8},
        {TSD_PERF_TEST_STEP_DATA, sizeof(rd_stable) - 8},
    };
    const tsd_perf_test_read_step_t stable_llc_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 2},
        {TSD_PERF_TEST_STEP_DATA, 6},
    };
    const tsd_perf_test_read_stream_t stable_streams[] = {
        {100, stable_group_steps, sizeof(stable_group_steps) / sizeof(stable_group_steps[0]),
         (const uint8_t *)&rd_stable, sizeof(rd_stable)},
        {101, stable_llc_steps, sizeof(stable_llc_steps) / sizeof(stable_llc_steps[0]),
         (const uint8_t *)&llc_stable, sizeof(llc_stable)},
    };
    tsd_test_perf_set_read_streams(stable_streams, sizeof(stable_streams) / sizeof(stable_streams[0]));
    memset(&eval, 0, sizeof(eval));
    triggered = tsd_perf_evaluate(ctx, &eval, NULL);
    tsd_test_perf_clear_read_streams();
    if (triggered != 0 || eval.cpi_milli != 1000 ||
        tsd_test_perf_get_last_llc_value(ctx) != llc_stable) {
        fprintf(stderr, "stable partial group read verification failed\n");
        rc = 1;
        goto out;
    }

    const perf_group_read_test_t rd_hot = {
        .nr = 2,
        .time_enabled = 300,
        .time_running = 300,
        .values = {7000, 4500},
    };
    const uint64_t llc_hot = 60;
    const tsd_perf_test_read_step_t hot_group_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 12},
        {TSD_PERF_TEST_STEP_DATA, sizeof(rd_hot) - 12},
    };
    const tsd_perf_test_read_step_t hot_llc_steps[] = {
        {TSD_PERF_TEST_STEP_EINTR, 0},
        {TSD_PERF_TEST_STEP_DATA, 5},
        {TSD_PERF_TEST_STEP_DATA, 3},
    };
    const tsd_perf_test_read_stream_t hot_streams[] = {
        {100, hot_group_steps, sizeof(hot_group_steps) / sizeof(hot_group_steps[0]),
         (const uint8_t *)&rd_hot, sizeof(rd_hot)},
        {101, hot_llc_steps, sizeof(hot_llc_steps) / sizeof(hot_llc_steps[0]),
         (const uint8_t *)&llc_hot, sizeof(llc_hot)},
    };
    tsd_test_perf_set_read_streams(hot_streams, sizeof(hot_streams) / sizeof(hot_streams[0]));
    memset(&eval, 0, sizeof(eval));
    triggered = tsd_perf_evaluate(ctx, &eval, NULL);
    tsd_test_perf_clear_read_streams();

    if (!tsd_test_perf_get_last_group_valid(ctx) ||
        tsd_test_perf_get_last_llc_value(ctx) != llc_hot ||
        eval.cpi_milli != 2000 || triggered == 0) {
        fprintf(stderr, "hot partial group read verification failed\n");
        rc = 1;
    }

out:
    tsd_test_perf_destroy_dummy_context(ctx);
    return rc;
}

static int run_telemetry_weight_test(void) {
    int rc = 0;
    perf_ctx_t *ctx = NULL;
    tsd_test_reset_runtime();
    tsd_test_set_policy_counts(1, 1);
    tsd_test_set_timing(1000, 1, 1, 1);
    g_tsd_config.thermal_temp_weight_milli = 20;
    g_tsd_config.thermal_ratio_weight_milli = 30;
    if (tsd_test_refresh_ticks() != 0) {
        fprintf(stderr, "failed to refresh ticks for telemetry test\n");
        rc = 1;
        goto out;
    }
    ctx = tsd_test_perf_create_dummy_context();
    if (!ctx) {
        fprintf(stderr, "failed to create dummy perf context for telemetry test\n");
        rc = 1;
        goto out;
    }
    tsd_test_perf_set_mode(ctx, TSD_PERF_MODE_SOFTWARE);
    const uint32_t ratios[] = {1400, 1400, 1400, 1400};
    tsd_test_set_fake_perf_script(ratios, sizeof(ratios) / sizeof(ratios[0]), 0);
    const tsd_telemetry_sample_t telemetry[] = {
        {.temp_available = 1, .freq_ratio_available = 1, .package_temp_millic = 95000, .freq_ratio_milli = 900},
        {.temp_available = 1, .freq_ratio_available = 1, .package_temp_millic = 90000, .freq_ratio_milli = 950},
        {.temp_available = 0, .freq_ratio_available = 0, .package_temp_millic = 0, .freq_ratio_milli = 0},
        {.temp_available = 0, .freq_ratio_available = 0, .package_temp_millic = 0, .freq_ratio_milli = 0},
    };
    tsd_test_set_fake_telemetry(telemetry, sizeof(telemetry) / sizeof(telemetry[0]));
    tsd_thermal_eval_t eval = {0};
    int triggered = 0;
    for (size_t i = 0; i < sizeof(ratios) / sizeof(ratios[0]); ++i) {
        if (tsd_perf_evaluate(ctx, &eval, &g_tsd_config)) {
            triggered = 1;
            break;
        }
    }
    if (!triggered) {
        fprintf(stderr, "telemetry weighting did not trigger severity\n");
        rc = 1;
        goto out;
    }
    if (eval.thermal_severity_milli != 203) {
        fprintf(stderr, "unexpected thermal severity (got %lu)\n", eval.thermal_severity_milli);
        rc = 1;
        goto out;
    }
    if (eval.severity_milli != eval.thermal_severity_milli) {
        fprintf(stderr, "overall severity did not match thermal component\n");
        rc = 1;
        goto out;
    }
    if (!eval.temp_available || eval.package_temp_millic != 95000) {
        fprintf(stderr, "temperature telemetry not propagated\n");
        rc = 1;
        goto out;
    }
    if (!eval.freq_ratio_available || eval.freq_ratio_milli != 900) {
        fprintf(stderr, "frequency telemetry not propagated\n");
        rc = 1;
        goto out;
    }

out:
    tsd_test_clear_fake_perf_script();
    tsd_test_set_fake_telemetry(NULL, 0);
    if (ctx) {
        tsd_test_perf_destroy_dummy_context(ctx);
    }
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
    if (run_telemetry_weight_test() != 0) {
        return 1;
    }
    if (run_monitor_thread_scenario() != 0) {
        return 1;
    }
    if (run_patch_failure_diagnostic() != 0) {
        return 1;
    }
    if (run_software_timeout_trip() != 0) {
        return 1;
    }
    return 0;
}