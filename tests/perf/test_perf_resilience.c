#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>

#include <thermal/simd/telemetry_fusion.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_perf.h>

static void test_runtime_group_failure_fails_closed(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);

    perf_ctx_t *ctx = tsd_perf_test_create_dummy_context();
    assert(ctx != NULL);
    tsd_perf_test_set_mode(ctx, TSD_PERF_MODE_HARDWARE);
    tsd_perf_test_set_group_fd(ctx, 123);

    const tsd_perf_test_read_step_t steps[] = {
        {TSD_PERF_TEST_STEP_DATA, 0},
    };
    const tsd_perf_test_read_stream_t stream = {
        .fd = 123,
        .steps = steps,
        .step_count = sizeof(steps) / sizeof(steps[0]),
        .data = NULL,
        .data_len = 0,
    };
    tsd_perf_test_set_read_streams(&stream, 1);

    tsd_thermal_eval_t eval = {0};
    int rc = tsd_perf_evaluate(ctx, &eval, &g_tsd_config);
    assert(rc != 0);
    assert(eval.severity_milli > 0);
    assert(tsd_perf_get_mode(ctx) == TSD_PERF_MODE_SOFTWARE);

    tsd_perf_test_clear_read_streams();
    tsd_perf_test_destroy_dummy_context(ctx);
}

static void test_selected_cpu_is_in_allowed_cpuset(void) {
    assert(setenv("TSD_FAKE_PERF", "1", 1) == 0);
    perf_ctx_t *ctx = tsd_perf_init(NULL);
    assert(ctx != NULL);

    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    assert(sched_getaffinity(0, sizeof(allowed), &allowed) == 0);
    int cpu = tsd_perf_get_pinned_cpu(ctx);
    assert(cpu >= 0);
    assert(CPU_ISSET(cpu, &allowed));

    tsd_perf_cleanup(ctx);
    unsetenv("TSD_FAKE_PERF");
}

static void test_fusion_is_reference_counted(void) {
    assert(tsd_telemetry_fusion_start_for_cpu(0) == 0);
    assert(tsd_telemetry_fusion_start_for_cpu(0) == 0);

    tsd_telemetry_sample_t sample = {0};
    sample.temp_available = 1;
    sample.package_temp_millic = 70000;
    sample.freq_ratio_available = 1;
    sample.freq_ratio_milli = 1000;
    assert(tsd_telemetry_fusion_publish_sample(&sample) == 0);

    tsd_telemetry_fusion_stop();
    sample.package_temp_millic = 71000;
    assert(tsd_telemetry_fusion_publish_sample(&sample) == 0);

    tsd_telemetry_fusion_stop();
    assert(tsd_telemetry_fusion_publish_sample(&sample) != 0);
}

int main(void) {
    test_runtime_group_failure_fails_closed();
    test_selected_cpu_is_in_allowed_cpuset();
    test_fusion_is_reference_counted();
    return 0;
}
