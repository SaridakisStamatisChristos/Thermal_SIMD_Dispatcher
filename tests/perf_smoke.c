#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_perf.h>

#define WORKLOAD_ITERS 1024
#define WARMUP_ITERATIONS 8
#define MEASURED_ITERATIONS 64

static void smoke_workload(void) {
    for (int i = 0; i < WORKLOAD_ITERS; ++i) {
        atomic_fetch_add_explicit(&g_tsd_workload_iterations, (uint64_t)1, memory_order_relaxed);
    }
}

int main(void) {
    if (setenv("TSD_FAKE_PERF", "1", 1) != 0) {
        perror("setenv");
        return 1;
    }

    tsd_runtime_config cfg;
    tsd_runtime_config_set_defaults(&cfg);
    cfg.work_iters = WORKLOAD_ITERS;
    tsd_runtime_config_refresh_ticks(&cfg);

    const uint32_t fake_ratios[] = {1000, 950, 925, 900, 875, 850};
    tsd_perf_set_fake_script(fake_ratios, sizeof(fake_ratios) / sizeof(fake_ratios[0]), 1200);

    perf_ctx_t *ctx = tsd_perf_init(smoke_workload);
    if (!ctx) {
        fprintf(stderr, "failed to initialise perf context\n");
        return 1;
    }

    tsd_perf_measure_baseline(ctx, &cfg);

    tsd_thermal_eval_t eval = {0};
    for (int i = 0; i < WARMUP_ITERATIONS; ++i) {
        (void)tsd_perf_evaluate(ctx, &eval, &cfg);
    }

    struct timespec start = {0}, end = {0};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < MEASURED_ITERATIONS; ++i) {
        (void)tsd_perf_evaluate(ctx, &eval, &cfg);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    const double start_us = (double)start.tv_sec * 1e6 + (double)start.tv_nsec / 1e3;
    const double end_us = (double)end.tv_sec * 1e6 + (double)end.tv_nsec / 1e3;
    const double elapsed = fmax(end_us - start_us, 1.0);
    const double per_eval = elapsed / (double)MEASURED_ITERATIONS;

    tsd_perf_clear_fake_script();
    tsd_perf_cleanup(ctx);

    printf("PERF_SMOKE_PER_EVAL_US=%.3f\n", per_eval);
    printf("PERF_SMOKE_ITERATIONS=%d\n", MEASURED_ITERATIONS);

    return 0;
}
