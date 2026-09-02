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
    size_t next_offset;
    size_t fail_offset;
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

static int run_v2(void *context, size_t offset, size_t work_items) {
    struct counters *state = (struct counters *)context;
    assert(offset == state->next_offset);
    if (offset == state->fail_offset) {
        return 73;
    }
    state->next_offset += work_items;
    state->sse += work_items;
    state->calls++;
    return 0;
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

    /* v2 passes exact offsets and propagates callback failures. */
    counters = (struct counters){.next_offset = 100, .fail_offset = SIZE_MAX};
    tsd_kernel_variants_v2_t variants_v2 = {
        .sse41 = run_v2,
        .context = &counters,
    };
    tsd_kernel_dispatch_v2_t *dispatch_v2 = NULL;
    assert(tsd_kernel_dispatch_v2_create(&variants_v2, &dispatch_v2) == 0);
    assert(tsd_kernel_dispatch_v2_execute_chunked(dispatch_v2, 100, 10, 3, &used) == 0);
    assert(counters.next_offset == 110);
    assert(counters.sse == 10);
    assert(counters.calls == 4);

    counters.fail_offset = 110;
    assert(tsd_kernel_dispatch_v2_execute(dispatch_v2, 110, 2, &used) == 73);
    assert(counters.sse == 10);
    assert(counters.calls == 4);
    assert(tsd_kernel_dispatch_v2_execute_chunked(dispatch_v2, SIZE_MAX, 1, 1, &used) != 0);
    tsd_kernel_dispatch_v2_destroy(dispatch_v2);
    return 0;
}
