#include <stddef.h>
#include <stdint.h>

#include <thermal/simd/adaptive_dispatch.h>
#include <thermal/simd/runtime.h>

static void sse41_kernel(void *context, size_t work_items) {
    uint64_t *counter = (uint64_t *)context;
    *counter += (uint64_t)work_items;
}

int main(void) {
    uint64_t counter = 0;
    tsd_kernel_variants_t variants = {
        .sse41 = sse41_kernel,
        .avx2 = NULL,
        .avx512 = NULL,
        .context = &counter,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    if (tsd_kernel_dispatch_create(&variants, &dispatch) != 0 || !dispatch) {
        return 1;
    }

    simd_width_t used = SIMD_AVX512;
    int rc = tsd_kernel_dispatch_execute(dispatch, 17, &used);
    tsd_kernel_dispatch_destroy(dispatch);
    if (rc != 0 || counter != 17 || used != SIMD_SSE41) {
        return 2;
    }

    /* Force the installed archive to resolve the exported lifecycle object too. */
    if (tsd_runtime_perf_mode(NULL) != TSD_PERF_MODE_NONE) {
        return 3;
    }
    return 0;
}
