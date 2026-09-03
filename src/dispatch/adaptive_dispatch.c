#include <thermal/simd/adaptive_dispatch.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_cpu.h>
#include <thermal/simd/thermal_perf.h>
#include <thermal/simd/thermal_trampoline.h>

struct tsd_kernel_dispatch {
    tsd_kernel_variants_t variants;
    simd_width_t host_max;
};

struct tsd_kernel_dispatch_v2 {
    tsd_kernel_variants_v2_t variants;
    simd_width_t host_max;
};

static simd_width_t detect_host_max(void) {
    tsd_runtime_config probe = {0};
    probe.allow_avx512 = 1;
    return tsd_detect_max_simd(&probe);
}

/*
 * Production width publication spans several legacy compatibility globals.
 * The patcher owns g_tsd_patch_lock across the complete transition, so readers
 * that need a coherent control-plane snapshot take the same lock rather than
 * observing an intermediate width/pointer publication state.
 */
static simd_width_t selected_width_snapshot(void) {
    int lock_rc = pthread_mutex_lock(&g_tsd_patch_lock);
    if (lock_rc != 0) {
        return SIMD_SSE41;
    }
    simd_width_t selected = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
    pthread_mutex_unlock(&g_tsd_patch_lock);
    return selected;
}

static int resolve_internal(const tsd_kernel_dispatch_t *dispatch,
                            simd_width_t *resolved,
                            tsd_kernel_fn *fn) {
    if (!dispatch || !dispatch->variants.sse41) {
        errno = EINVAL;
        return -1;
    }

    simd_width_t selected = selected_width_snapshot();
    if (selected < SIMD_SSE41 || selected > SIMD_AVX512) {
        selected = SIMD_SSE41;
    }
    if (selected > dispatch->host_max) {
        selected = dispatch->host_max;
    }

    simd_width_t actual = SIMD_SSE41;
    tsd_kernel_fn chosen = dispatch->variants.sse41;
    if (selected >= SIMD_AVX512 && dispatch->host_max >= SIMD_AVX512 && dispatch->variants.avx512) {
        actual = SIMD_AVX512;
        chosen = dispatch->variants.avx512;
    } else if (selected >= SIMD_AVX2 && dispatch->host_max >= SIMD_AVX2 && dispatch->variants.avx2) {
        actual = SIMD_AVX2;
        chosen = dispatch->variants.avx2;
    }

    if (resolved) {
        *resolved = actual;
    }
    if (fn) {
        *fn = chosen;
    }
    return 0;
}

int tsd_kernel_dispatch_create(const tsd_kernel_variants_t *variants,
                               tsd_kernel_dispatch_t **out_dispatch) {
    if (!variants || !out_dispatch || !variants->sse41) {
        errno = EINVAL;
        return -1;
    }
    *out_dispatch = NULL;
    if (!tsd_cpu_has_sse41()) {
        errno = ENOTSUP;
        return -1;
    }

    tsd_kernel_dispatch_t *dispatch = calloc(1, sizeof(*dispatch));
    if (!dispatch) {
        return -1;
    }
    dispatch->variants = *variants;
    dispatch->host_max = detect_host_max();
    if (dispatch->host_max < SIMD_SSE41 || dispatch->host_max > SIMD_AVX512) {
        dispatch->host_max = SIMD_SSE41;
    }
    *out_dispatch = dispatch;
    return 0;
}

void tsd_kernel_dispatch_destroy(tsd_kernel_dispatch_t *dispatch) {
    free(dispatch);
}

int tsd_kernel_dispatch_resolve(const tsd_kernel_dispatch_t *dispatch,
                                simd_width_t *resolved_width) {
    if (!resolved_width) {
        errno = EINVAL;
        return -1;
    }
    return resolve_internal(dispatch, resolved_width, NULL);
}

int tsd_kernel_dispatch_execute(tsd_kernel_dispatch_t *dispatch,
                                size_t work_items,
                                simd_width_t *used_width) {
    tsd_kernel_fn fn = NULL;
    simd_width_t actual = SIMD_SSE41;
    if (resolve_internal(dispatch, &actual, &fn) != 0 || !fn) {
        return -1;
    }

    fn(dispatch->variants.context, work_items);
    if (work_items > 0) {
        atomic_fetch_add_explicit(&g_tsd_workload_iterations,
                                  (uint64_t)work_items,
                                  memory_order_relaxed);
    }
    if (used_width) {
        *used_width = actual;
    }
    return 0;
}

int tsd_kernel_dispatch_execute_chunked(tsd_kernel_dispatch_t *dispatch,
                                        size_t total_work_items,
                                        size_t max_chunk_items,
                                        simd_width_t *last_used_width) {
    if (!dispatch || max_chunk_items == 0) {
        errno = EINVAL;
        return -1;
    }

    if (total_work_items == 0) {
        if (last_used_width) {
            if (tsd_kernel_dispatch_resolve(dispatch, last_used_width) != 0) {
                return -1;
            }
        }
        return 0;
    }

    size_t remaining = total_work_items;
    simd_width_t last = SIMD_SSE41;
    while (remaining > 0) {
        size_t chunk = remaining < max_chunk_items ? remaining : max_chunk_items;
        if (tsd_kernel_dispatch_execute(dispatch, chunk, &last) != 0) {
            return -1;
        }
        remaining -= chunk;
    }

    if (last_used_width) {
        *last_used_width = last;
    }
    return 0;
}

static int resolve_v2_internal(const tsd_kernel_dispatch_v2_t *dispatch,
                               simd_width_t *resolved,
                               tsd_kernel_fn_v2 *fn) {
    if (!dispatch || !dispatch->variants.sse41) {
        errno = EINVAL;
        return -1;
    }

    simd_width_t selected = selected_width_snapshot();
    if (selected < SIMD_SSE41 || selected > SIMD_AVX512) {
        selected = SIMD_SSE41;
    }
    if (selected > dispatch->host_max) {
        selected = dispatch->host_max;
    }

    simd_width_t actual = SIMD_SSE41;
    tsd_kernel_fn_v2 chosen = dispatch->variants.sse41;
    if (selected >= SIMD_AVX512 && dispatch->host_max >= SIMD_AVX512 && dispatch->variants.avx512) {
        actual = SIMD_AVX512;
        chosen = dispatch->variants.avx512;
    } else if (selected >= SIMD_AVX2 && dispatch->host_max >= SIMD_AVX2 && dispatch->variants.avx2) {
        actual = SIMD_AVX2;
        chosen = dispatch->variants.avx2;
    }
    if (resolved) {
        *resolved = actual;
    }
    if (fn) {
        *fn = chosen;
    }
    return 0;
}

int tsd_kernel_dispatch_v2_create(const tsd_kernel_variants_v2_t *variants,
                                  tsd_kernel_dispatch_v2_t **out_dispatch) {
    if (!variants || !out_dispatch || !variants->sse41) {
        errno = EINVAL;
        return -1;
    }
    *out_dispatch = NULL;
    if (!tsd_cpu_has_sse41()) {
        errno = ENOTSUP;
        return -1;
    }
    tsd_kernel_dispatch_v2_t *dispatch = calloc(1, sizeof(*dispatch));
    if (!dispatch) {
        return -1;
    }
    dispatch->variants = *variants;
    dispatch->host_max = detect_host_max();
    if (dispatch->host_max < SIMD_SSE41 || dispatch->host_max > SIMD_AVX512) {
        dispatch->host_max = SIMD_SSE41;
    }
    *out_dispatch = dispatch;
    return 0;
}

void tsd_kernel_dispatch_v2_destroy(tsd_kernel_dispatch_v2_t *dispatch) {
    free(dispatch);
}

int tsd_kernel_dispatch_v2_resolve(const tsd_kernel_dispatch_v2_t *dispatch,
                                   simd_width_t *resolved_width) {
    if (!resolved_width) {
        errno = EINVAL;
        return -1;
    }
    return resolve_v2_internal(dispatch, resolved_width, NULL);
}

int tsd_kernel_dispatch_v2_execute(tsd_kernel_dispatch_v2_t *dispatch,
                                   size_t offset,
                                   size_t work_items,
                                   simd_width_t *used_width) {
    tsd_kernel_fn_v2 fn = NULL;
    simd_width_t actual = SIMD_SSE41;
    if (resolve_v2_internal(dispatch, &actual, &fn) != 0 || !fn) {
        return -1;
    }
    int rc = fn(dispatch->variants.context, offset, work_items);
    if (rc != 0) {
        return rc;
    }
    if (work_items > 0) {
        atomic_fetch_add_explicit(&g_tsd_workload_iterations,
                                  (uint64_t)work_items,
                                  memory_order_relaxed);
    }
    if (used_width) {
        *used_width = actual;
    }
    return 0;
}

int tsd_kernel_dispatch_v2_execute_chunked(tsd_kernel_dispatch_v2_t *dispatch,
                                           size_t initial_offset,
                                           size_t total_work_items,
                                           size_t max_chunk_items,
                                           simd_width_t *last_used_width) {
    if (!dispatch || max_chunk_items == 0 || initial_offset > SIZE_MAX - total_work_items) {
        errno = EINVAL;
        return -1;
    }
    if (total_work_items == 0) {
        return last_used_width ? tsd_kernel_dispatch_v2_resolve(dispatch, last_used_width) : 0;
    }

    size_t offset = initial_offset;
    size_t remaining = total_work_items;
    simd_width_t last = SIMD_SSE41;
    while (remaining > 0) {
        size_t chunk = remaining < max_chunk_items ? remaining : max_chunk_items;
        int rc = tsd_kernel_dispatch_v2_execute(dispatch, offset, chunk, &last);
        if (rc != 0) {
            return rc;
        }
        offset += chunk;
        remaining -= chunk;
    }
    if (last_used_width) {
        *last_used_width = last;
    }
    return 0;
}
