#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <thermal/simd/adaptive_dispatch.h>
#include <thermal/simd/thermal_trampoline.h>

typedef struct benchmark_state_s {
    volatile uint64_t checksum;
} benchmark_state_t;

static int tiny_kernel(void *context, size_t offset, size_t work_items) {
    benchmark_state_t *state = (benchmark_state_t *)context;
    state->checksum += (uint64_t)offset + (uint64_t)work_items;
    return 0;
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static size_t parse_iterations(int argc, char **argv) {
    if (argc == 1) {
        return 10000000U;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s [iterations]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(argv[1], &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0 || value > SIZE_MAX) {
        fprintf(stderr, "invalid iteration count: %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }
    return (size_t)value;
}

int main(int argc, char **argv) {
    const size_t iterations = parse_iterations(argc, argv);
    benchmark_state_t direct = {0};
    benchmark_state_t dispatched = {0};

    if (tsd_trampoline_init() != 0 || tsd_trampoline_patch(SIMD_SSE41) != 0) {
        perror("trampoline initialization");
        return EXIT_FAILURE;
    }
    tsd_kernel_variants_v2_t variants = {
        .sse41 = tiny_kernel,
        .context = &dispatched,
    };
    tsd_kernel_dispatch_v2_t *dispatch = NULL;
    if (tsd_kernel_dispatch_v2_create(&variants, &dispatch) != 0) {
        perror("dispatch creation");
        return EXIT_FAILURE;
    }

    /* Warm caches and branch predictors before collecting either sample. */
    for (size_t i = 0; i < 10000U; ++i) {
        (void)tiny_kernel(&direct, i, 1);
        if (tsd_kernel_dispatch_v2_execute(dispatch, i, 1, NULL) != 0) {
            return EXIT_FAILURE;
        }
    }
    direct.checksum = 0;
    dispatched.checksum = 0;

    uint64_t start = monotonic_ns();
    for (size_t i = 0; i < iterations; ++i) {
        (void)tiny_kernel(&direct, i, 1);
    }
    uint64_t direct_ns = monotonic_ns() - start;

    start = monotonic_ns();
    for (size_t i = 0; i < iterations; ++i) {
        if (tsd_kernel_dispatch_v2_execute(dispatch, i, 1, NULL) != 0) {
            fprintf(stderr, "dispatch failed at iteration %zu\n", i);
            return EXIT_FAILURE;
        }
    }
    uint64_t dispatch_ns = monotonic_ns() - start;
    tsd_kernel_dispatch_v2_destroy(dispatch);

    if (direct.checksum != dispatched.checksum) {
        fprintf(stderr, "checksum mismatch: direct=%" PRIu64 " dispatch=%" PRIu64 "\n",
                direct.checksum, dispatched.checksum);
        return EXIT_FAILURE;
    }

    double direct_per_call = (double)direct_ns / (double)iterations;
    double dispatch_per_call = (double)dispatch_ns / (double)iterations;
    printf("iterations=%zu direct_ns_per_call=%.3f dispatch_ns_per_call=%.3f "
           "incremental_ns_per_call=%.3f checksum=%" PRIu64 "\n",
           iterations, direct_per_call, dispatch_per_call,
           dispatch_per_call - direct_per_call, direct.checksum);
    return EXIT_SUCCESS;
}
