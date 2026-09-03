#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <thermal/simd/telemetry_fusion.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_perf.h>

static int cpu_count(const cpu_set_t *set) {
    int count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, set)) {
            ++count;
        }
    }
    return count;
}

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

static void test_software_mode_never_authorizes_upgrades(void) {
    perf_ctx_t *ctx = tsd_perf_test_create_dummy_context();
    assert(ctx != NULL);

    tsd_perf_test_set_mode(ctx, TSD_PERF_MODE_SOFTWARE);
    assert(tsd_perf_upgrades_allowed(ctx) == 0);

    /* The legacy environment escape hatch is intentionally ignored. */
    assert(setenv("TSD_ALLOW_SOFTWARE_UPGRADES", "1", 1) == 0);
    assert(tsd_perf_upgrades_allowed(ctx) == 0);
    unsetenv("TSD_ALLOW_SOFTWARE_UPGRADES");

    tsd_perf_test_set_mode(ctx, TSD_PERF_MODE_HARDWARE);
    assert(tsd_perf_upgrades_allowed(ctx) == 1);
    tsd_perf_test_destroy_dummy_context(ctx);
}

static void test_group_progress_requires_actual_runtime(void) {
    assert(tsd_perf_test_group_progress_valid(100, 80, 1000, 500,
                                              200, 160, 2200, 1400) == 1);
    assert(tsd_perf_test_group_progress_valid(100, 80, 1000, 500,
                                              200, 80, 2200, 1400) == 0);
    assert(tsd_perf_test_group_progress_valid(100, 80, 1000, 500,
                                              200, 160, 2200, 500) == 0);
    assert(tsd_perf_test_group_progress_valid(100, 80, 1000, 500,
                                              90, 160, 2200, 1400) == 0);
}

static void test_cpuset_selection_and_affinity_restoration(void) {
    cpu_set_t original;
    CPU_ZERO(&original);
    assert(sched_getaffinity(0, sizeof(original), &original) == 0);
    int original_count = cpu_count(&original);
    assert(original_count > 0);

    assert(setenv("TSD_FAKE_PERF", "1", 1) == 0);
    perf_ctx_t *ctx = tsd_perf_init(NULL);
    assert(ctx != NULL);

    int workload_cpu = tsd_perf_get_pinned_cpu(ctx);
    int monitor_cpu = tsd_perf_get_monitor_cpu(ctx);
    assert(workload_cpu >= 0);
    assert(monitor_cpu >= 0);
    assert(CPU_ISSET(workload_cpu, &original));
    assert(CPU_ISSET(monitor_cpu, &original));
    if (original_count > 1) {
        assert(monitor_cpu != workload_cpu);
    }

    cpu_set_t pinned;
    CPU_ZERO(&pinned);
    assert(sched_getaffinity(0, sizeof(pinned), &pinned) == 0);
    assert(cpu_count(&pinned) == 1);
    assert(CPU_ISSET(workload_cpu, &pinned));

    tsd_perf_cleanup(ctx);
    unsetenv("TSD_FAKE_PERF");

    cpu_set_t restored;
    CPU_ZERO(&restored);
    assert(sched_getaffinity(0, sizeof(restored), &restored) == 0);
    assert(memcmp(&restored, &original, sizeof(cpu_set_t)) == 0);
}

static void test_fusion_is_reference_counted_and_cpu_coherent(void) {
    assert(tsd_telemetry_fusion_start_for_cpu(0) == 0);
    assert(tsd_telemetry_fusion_start_for_cpu(0) == 0);
    assert(tsd_telemetry_fusion_start_for_cpu(1) != 0);

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
    test_software_mode_never_authorizes_upgrades();
    test_group_progress_requires_actual_runtime();
    test_cpuset_selection_and_affinity_restoration();
    test_fusion_is_reference_counted_and_cpu_coherent();
    return 0;
}
