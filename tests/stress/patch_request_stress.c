#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stress_common.h"

typedef struct {
    int thread_id;
    int iterations;
    const simd_width_t *widths;
    size_t width_count;
    atomic_uint *successes;
    atomic_uint *unexpected_failures;
    atomic_uint *expected_failures;
} patch_thread_arg_t;

static int parse_positive(const char *value, int fallback) {
    if (!value) {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0 || parsed > INT_MAX) {
        return fallback;
    }
    return (int)parsed;
}

static int resolve_iterations(int argc, char **argv) {
    int iterations = 2000;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = parse_positive(argv[++i], iterations);
        } else if (strncmp(argv[i], "--iterations=", 13) == 0) {
            iterations = parse_positive(argv[i] + 13, iterations);
        }
    }
    const char *scale = getenv("TSD_STRESS_SCALE");
    if (scale) {
        double multiplier = strtod(scale, NULL);
        if (multiplier > 0.0) {
            long scaled = (long)(iterations * multiplier);
            if (scaled > iterations) {
                iterations = (int)(scaled > INT_MAX ? INT_MAX : scaled);
            }
        }
    }
    return iterations;
}

static int resolve_threads(int argc, char **argv) {
    int threads = 4;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = parse_positive(argv[++i], threads);
        } else if (strncmp(argv[i], "--threads=", 10) == 0) {
            threads = parse_positive(argv[i] + 10, threads);
        }
    }
    return threads > 0 ? threads : 1;
}

static void *patch_thread_fn(void *data) {
    patch_thread_arg_t *arg = (patch_thread_arg_t *)data;
    for (int iter = 0; iter < arg->iterations; ++iter) {
        for (size_t w = 0; w < arg->width_count; ++w) {
            simd_width_t width = arg->widths[w];
            if (arg->thread_id == 0 && (iter % 64 == 0)) {
                int rc = tsd_stress_inject_failure(TSD_PATCH_FAIL_PROTECT_WRITE, width);
                if (rc != 0) {
                    atomic_fetch_add_explicit(arg->expected_failures, 1U, memory_order_relaxed);
                    continue;
                }
            }
            int rc = tsd_stress_patch(width);
            if (rc == 0) {
                atomic_fetch_add_explicit(arg->successes, 1U, memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(arg->unexpected_failures, 1U, memory_order_relaxed);
            }
        }
        tsd_test_run_workload(256);
    }
    return NULL;
}

static void print_summary(unsigned int threads,
                          unsigned int iterations,
                          unsigned int successes,
                          unsigned int expected_failures,
                          unsigned int unexpected_failures,
                          double elapsed) {
    printf("patch_request_stress summary\n");
    printf("  threads: %u\n", threads);
    printf("  iterations_per_thread: %u\n", iterations);
    printf("  successes: %u\n", successes);
    printf("  expected_failures: %u\n", expected_failures);
    printf("  unexpected_failures: %u\n", unexpected_failures);
    printf("  duration_seconds: %.3f\n", elapsed);
    fflush(stdout);
}

int main(int argc, char **argv) {
    int iterations = resolve_iterations(argc, argv);
    int thread_count = resolve_threads(argc, argv);
    if (thread_count > 64) {
        thread_count = 64;
    }
    if (tsd_stress_prepare_runtime() != 0) {
        return EXIT_FAILURE;
    }

    const simd_width_t widths[] = {SIMD_SSE41, SIMD_AVX2, SIMD_AVX512};
    atomic_uint successes = ATOMIC_VAR_INIT(0);
    atomic_uint expected_failures = ATOMIC_VAR_INIT(0);
    atomic_uint unexpected_failures = ATOMIC_VAR_INIT(0);

    pthread_t *threads = calloc((size_t)thread_count, sizeof(pthread_t));
    patch_thread_arg_t *args = calloc((size_t)thread_count, sizeof(patch_thread_arg_t));
    if (!threads || !args) {
        fprintf(stderr, "failed to allocate thread metadata\n");
        free(threads);
        free(args);
        tsd_stress_teardown_runtime();
        return EXIT_FAILURE;
    }

    struct timespec start_ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    for (int i = 0; i < thread_count; ++i) {
        args[i].thread_id = i;
        args[i].iterations = iterations;
        args[i].widths = widths;
        args[i].width_count = sizeof(widths) / sizeof(widths[0]);
        args[i].successes = &successes;
        args[i].unexpected_failures = &unexpected_failures;
        args[i].expected_failures = &expected_failures;
        if (pthread_create(&threads[i], NULL, patch_thread_fn, &args[i]) != 0) {
            fprintf(stderr, "failed to create patch worker %d\n", i);
            thread_count = i;
            break;
        }
    }

    for (int i = 0; i < thread_count; ++i) {
        pthread_join(threads[i], NULL);
    }

    struct timespec end_ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    double elapsed = (end_ts.tv_sec - start_ts.tv_sec) +
                     (end_ts.tv_nsec - start_ts.tv_nsec) / 1e9;

    unsigned int success_count = atomic_load_explicit(&successes, memory_order_relaxed);
    unsigned int expected_failure_count = atomic_load_explicit(&expected_failures, memory_order_relaxed);
    unsigned int unexpected_failure_count = atomic_load_explicit(&unexpected_failures, memory_order_relaxed);

    print_summary((unsigned int)thread_count,
                  (unsigned int)iterations,
                  success_count,
                  expected_failure_count,
                  unexpected_failure_count,
                  elapsed);

    free(threads);
    free(args);
    tsd_stress_teardown_runtime();

    return unexpected_failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
