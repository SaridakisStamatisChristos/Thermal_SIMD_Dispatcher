#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <thermal/simd/telemetry_helper.h>

#include "stress_common.h"

typedef struct {
    const char *name;
    const tsd_telemetry_sample_t *samples;
    size_t count;
    useconds_t dwell_us;
} telemetry_scenario_t;

static const tsd_telemetry_sample_t telemetry_spike[] = {
    {.temp_available = 1, .freq_ratio_available = 1, .package_temp_millic = 125000, .freq_ratio_milli = 900},
    {.temp_available = 1, .freq_ratio_available = 1, .package_temp_millic = 112000, .freq_ratio_milli = 850},
};

static const tsd_telemetry_sample_t telemetry_dropout[] = {
    {.temp_available = 0, .freq_ratio_available = 1, .package_temp_millic = 0, .freq_ratio_milli = 400},
    {.temp_available = 1, .freq_ratio_available = 0, .package_temp_millic = 101000, .freq_ratio_milli = 0},
};

static const tsd_telemetry_sample_t telemetry_invalid[] = {
    {.temp_available = 1, .freq_ratio_available = 1, .package_temp_millic = -5000, .freq_ratio_milli = 2500},
    {.temp_available = 1, .freq_ratio_available = 1, .package_temp_millic = 145000, .freq_ratio_milli = 50},
};

static telemetry_scenario_t scenarios[] = {
    {.name = "thermal_spike", .samples = telemetry_spike, .count = sizeof(telemetry_spike) / sizeof(telemetry_spike[0]), .dwell_us = 50000},
    {.name = "telemetry_dropout", .samples = telemetry_dropout, .count = sizeof(telemetry_dropout) / sizeof(telemetry_dropout[0]), .dwell_us = 50000},
    {.name = "invalid_payload", .samples = telemetry_invalid, .count = sizeof(telemetry_invalid) / sizeof(telemetry_invalid[0]), .dwell_us = 50000},
};

static int parse_cycles(int argc, char **argv) {
    int cycles = 3;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            cycles = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--cycles=", 9) == 0) {
            cycles = atoi(argv[i] + 9);
        }
    }
    const char *env_cycles = getenv("TSD_FAULT_CYCLES");
    if (env_cycles) {
        int parsed = atoi(env_cycles);
        if (parsed > 0) {
            cycles = parsed;
        }
    }
    if (cycles <= 0) {
        cycles = 1;
    }
    return cycles;
}

static void run_scenario(const telemetry_scenario_t *scenario) {
    tsd_test_set_fake_telemetry(scenario->samples, scenario->count);
    tsd_test_run_workload(2048);
    usleep(scenario->dwell_us);
}

int main(int argc, char **argv) {
    int cycles = parse_cycles(argc, argv);

    if (tsd_stress_prepare_runtime() != 0) {
        return EXIT_FAILURE;
    }

    perf_ctx_t *ctx = tsd_test_init_perf();
    if (!ctx) {
        fprintf(stderr, "failed to initialize perf context\n");
        tsd_stress_teardown_runtime();
        return EXIT_FAILURE;
    }

    tsd_test_measure_baseline(ctx);

    const uint32_t perf_script[] = {2200, 2100, 2000, 1800, 1750, 1650};
    tsd_test_set_fake_perf_script(perf_script, sizeof(perf_script) / sizeof(perf_script[0]), 5);

    atomic_uint fault_injections = ATOMIC_VAR_INIT(0);

    tsd_test_set_running(1);
    pthread_t monitor_thread;
    if (pthread_create(&monitor_thread, NULL, thermal_monitor_thread, ctx) != 0) {
        fprintf(stderr, "failed to start monitor thread\n");
        tsd_test_set_running(0);
        tsd_test_cleanup_perf(ctx);
        tsd_stress_teardown_runtime();
        return EXIT_FAILURE;
    }

    for (int cycle = 0; cycle < cycles; ++cycle) {
        for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
            const telemetry_scenario_t *scenario = &scenarios[i];
            printf("telemetry_faults injecting=%s cycle=%d\n", scenario->name, cycle);
            fflush(stdout);
            run_scenario(scenario);
            atomic_fetch_add_explicit(&fault_injections, 1U, memory_order_relaxed);
        }
    }

    tsd_test_set_fake_telemetry(NULL, 0);
    usleep(50000);

    tsd_test_set_running(0);
    pthread_join(monitor_thread, NULL);

    unsigned int injected = atomic_load_explicit(&fault_injections, memory_order_relaxed);
    int last_group_valid = tsd_test_perf_get_last_group_valid(ctx);
    simd_width_t final_width = tsd_test_current_width();

    printf("telemetry_faults summary\n");
    printf("  cycles: %d\n", cycles);
    printf("  injections: %u\n", injected);
    printf("  last_group_valid: %d\n", last_group_valid);
    printf("  final_width: %d\n", (int)final_width);
    fflush(stdout);

    tsd_test_cleanup_perf(ctx);
    tsd_stress_teardown_runtime();

    int rc = 0;
    if (!last_group_valid) {
        fprintf(stderr, "perf group reported invalid data after telemetry faults\n");
        rc = 1;
    }
    if (final_width != SIMD_SSE41) {
        fprintf(stderr, "dispatcher width changed unexpectedly under telemetry faults\n");
        rc = 1;
    }
    if (injected == 0U) {
        fprintf(stderr, "no telemetry faults were injected\n");
        rc = 1;
    }

    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
