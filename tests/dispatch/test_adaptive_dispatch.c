#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <thermal/simd/adaptive_dispatch.h>
#include <thermal/simd/thermal_trampoline.h>

struct counters {
    uint64_t sse;
    uint64_t avx2;
    uint64_t avx512;
    size_t calls;
};

static void run_sse(void *context, size_t work_items) {
    struct counters *state = (struct counters *)context;
    state->sse += work_items;
    state->calls++;
}

static void run_avx2(void *context, size_t work_items) {
    struct counters *state = (struct counters *)context;
    state->avx2 += work_items;
    state->calls++;
}

static void run_avx512(void *context, size_t work_items) {
    struct counters *state = (struct counters *)context;
    state->avx512 += work_items;
    state->calls++;
}

int main(void) {
    assert(tsd_trampoline_init() == 0);
    assert(tsd_trampoline_patch(SIMD_SSE41) == 0);

    struct counters counters = {0};
    tsd_kernel_variants_t variants = {
        .sse41 = run_sse,
        .avx2 = run_avx2,
        .avx512 = run_avx512,
        .context = &counters,
    };

    tsd_kernel_dispatch_t *dispatch = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);
    assert(dispatch != NULL);

    simd_width_t used = SIMD_AVX512;
    assert(tsd_kernel_dispatch_execute(dispatch, 7, &used) == 0);
    assert(used == SIMD_SSE41);
    assert(counters.sse == 7);
    assert(counters.calls == 1);

    /* Requests are host-clamped; whichever callback is selected must match used. */
    assert(tsd_trampoline_patch(SIMD_AVX512) == 0);
    assert(tsd_kernel_dispatch_execute(dispatch, 11, &used) == 0);
    if (used == SIMD_AVX512) {
        assert(counters.avx512 == 11);
    } else if (used == SIMD_AVX2) {
        assert(counters.avx2 == 11);
    } else {
        assert(used == SIMD_SSE41);
        assert(counters.sse == 18);
    }
    assert(counters.calls == 2);

    /* Chunking must bound each call and preserve the exact work total. */
    counters = (struct counters){0};
    assert(tsd_trampoline_patch(SIMD_SSE41) == 0);
    assert(tsd_kernel_dispatch_execute_chunked(dispatch, 10, 3, &used) == 0);
    assert(used == SIMD_SSE41);
    assert(counters.sse == 10);
    assert(counters.calls == 4);

    size_t before_calls = counters.calls;
    assert(tsd_kernel_dispatch_execute_chunked(dispatch, 0, 3, &used) == 0);
    assert(counters.calls == before_calls);
    assert(tsd_kernel_dispatch_execute_chunked(dispatch, 1, 0, &used) != 0);

    tsd_kernel_dispatch_destroy(dispatch);

    /* Missing higher variants must conservatively fall back to SSE4.1. */
    counters = (struct counters){0};
    variants.avx2 = NULL;
    variants.avx512 = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);
    assert(tsd_kernel_dispatch_execute(dispatch, 5, &used) == 0);
    assert(used == SIMD_SSE41);
    assert(counters.sse == 5);
    tsd_kernel_dispatch_destroy(dispatch);

    variants.sse41 = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) != 0);
    return 0;
}
