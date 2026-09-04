#include <thermal/simd/adaptive_dispatch.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include <thermal/simd/runtime.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_cpu.h>
#include <thermal/simd/thermal_perf.h>
#include <thermal/simd/thermal_trampoline.h>

#include "../runtime_guard_internal.h"

struct tsd_kernel_dispatch {
    tsd_kernel_variants_t variants;
    simd_width_t host_max;
};

struct tsd_kernel_dispatch_v2 {
    tsd_kernel_variants_v2_t variants;
    simd_width_t host_max;
};

struct execution_cleanup {
    simd_width_t width;
    int previous_cancel_state;
    int restore_cancel_state;
};

static void execution_cleanup_handler(void *opaque) {
    struct execution_cleanup *cleanup = (struct execution_cleanup *)opaque;
    if (!cleanup) return;

    /* Release admission accounting before re-enabling cancellation. A pending
     * cancellation may take effect as soon as the old state is restored. */
    tsd_runtime_execution_leave(cleanup->width);
    if (cleanup->restore_cancel_state) {
        (void)pthread_setcancelstate(cleanup->previous_cancel_state, NULL);
    }
}

static int cancel_disable(int *previous_state) {
    if (!previous_state) return 0;
    return pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, previous_state) == 0;
}

static void cancel_restore_if_needed(int restore, int previous_state) {
    if (restore) (void)pthread_setcancelstate(previous_state, NULL);
}

static simd_width_t detect_host_max(void) {
    tsd_runtime_config probe = {0};
    probe.allow_avx512 = 1;
    return tsd_detect_max_simd(&probe);
}

/*
 * Resolution is advisory and never holds a synchronization primitive across
 * application code. Wide execution performs a second atomic admission check
 * immediately before invoking the selected kernel.
 */
static simd_width_t selected_width_snapshot(void) {
    simd_width_t selected = tsd_trampoline_state_current_width();
    if (selected < SIMD_SSE41 || selected > SIMD_AVX512) return SIMD_SSE41;
    if (selected > SIMD_SSE41 &&
        (!tsd_runtime_wide_admission_is_open() ||
         !tsd_runtime_current_thread_is_owner() ||
         !tsd_runtime_width_authorized(selected))) {
        return SIMD_SSE41;
    }
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
    if (selected > dispatch->host_max) selected = dispatch->host_max;

    simd_width_t actual = SIMD_SSE41;
    tsd_kernel_fn chosen = dispatch->variants.sse41;
    if (selected >= SIMD_AVX512 && dispatch->host_max >= SIMD_AVX512 && dispatch->variants.avx512) {
        actual = SIMD_AVX512;
        chosen = dispatch->variants.avx512;
    } else if (selected >= SIMD_AVX2 && dispatch->host_max >= SIMD_AVX2 && dispatch->variants.avx2) {
        actual = SIMD_AVX2;
        chosen = dispatch->variants.avx2;
    }

    if (resolved) *resolved = actual;
    if (fn) *fn = chosen;
    return 0;
}

static int enter_or_fallback_v1(tsd_kernel_dispatch_t *dispatch,
                                simd_width_t *actual,
                                tsd_kernel_fn *fn) {
    if (!dispatch || !actual || !fn || !*fn) {
        errno = EINVAL;
        return -1;
    }
    if (tsd_runtime_execution_enter(*actual) == 0) return 0;
    if (*actual <= SIMD_SSE41 || errno != EAGAIN) return -1;

    /* A guard update raced resolution. Fall back locally; do not make callers
     * retry and do not wait for a process-global writer. */
    *actual = SIMD_SSE41;
    *fn = dispatch->variants.sse41;
    return tsd_runtime_execution_enter(SIMD_SSE41);
}

static void account_completed_work(size_t work_items) {
    if (work_items > 0 && tsd_runtime_work_accounting_allowed()) {
        atomic_fetch_add_explicit(&g_tsd_workload_iterations,
                                  (uint64_t)work_items,
                                  memory_order_relaxed);
    }
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
    if (!dispatch) return -1;
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
    int previous_cancel_state = PTHREAD_CANCEL_ENABLE;
    int restore_cancel_state = cancel_disable(&previous_cancel_state);

    if (resolve_internal(dispatch, &actual, &fn) != 0 || !fn) {
        cancel_restore_if_needed(restore_cancel_state, previous_cancel_state);
        return -1;
    }
    if (enter_or_fallback_v1(dispatch, &actual, &fn) != 0) {
        cancel_restore_if_needed(restore_cancel_state, previous_cancel_state);
        return -1;
    }

    struct execution_cleanup cleanup = {
        .width = actual,
        .previous_cancel_state = previous_cancel_state,
        .restore_cancel_state = restore_cancel_state,
    };
    pthread_cleanup_push(execution_cleanup_handler, &cleanup);
    fn(dispatch->variants.context, work_items);
    account_completed_work(work_items);
    if (used_width) *used_width = actual;
    pthread_cleanup_pop(1);
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
        return last_used_width ? tsd_kernel_dispatch_resolve(dispatch, last_used_width) : 0;
    }

    size_t remaining = total_work_items;
    simd_width_t last = SIMD_SSE41;
    while (remaining > 0) {
        size_t chunk = remaining < max_chunk_items ? remaining : max_chunk_items;
        if (tsd_kernel_dispatch_execute(dispatch, chunk, &last) != 0) return -1;
        remaining -= chunk;
    }

    if (last_used_width) *last_used_width = last;
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
    if (selected > dispatch->host_max) selected = dispatch->host_max;

    simd_width_t actual = SIMD_SSE41;
    tsd_kernel_fn_v2 chosen = dispatch->variants.sse41;
    if (selected >= SIMD_AVX512 && dispatch->host_max >= SIMD_AVX512 && dispatch->variants.avx512) {
        actual = SIMD_AVX512;
        chosen = dispatch->variants.avx512;
    } else if (selected >= SIMD_AVX2 && dispatch->host_max >= SIMD_AVX2 && dispatch->variants.avx2) {
        actual = SIMD_AVX2;
        chosen = dispatch->variants.avx2;
    }
    if (resolved) *resolved = actual;
    if (fn) *fn = chosen;
    return 0;
}

static int enter_or_fallback_v2(tsd_kernel_dispatch_v2_t *dispatch,
                                simd_width_t *actual,
                                tsd_kernel_fn_v2 *fn) {
    if (!dispatch || !actual || !fn || !*fn) {
        errno = EINVAL;
        return -1;
    }
    if (tsd_runtime_execution_enter(*actual) == 0) return 0;
    if (*actual <= SIMD_SSE41 || errno != EAGAIN) return -1;
    *actual = SIMD_SSE41;
    *fn = dispatch->variants.sse41;
    return tsd_runtime_execution_enter(SIMD_SSE41);
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
    if (!dispatch) return -1;
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
    int previous_cancel_state = PTHREAD_CANCEL_ENABLE;
    int restore_cancel_state = cancel_disable(&previous_cancel_state);
    int callback_rc = -1;

    if (resolve_v2_internal(dispatch, &actual, &fn) != 0 || !fn) {
        cancel_restore_if_needed(restore_cancel_state, previous_cancel_state);
        return -1;
    }
    if (enter_or_fallback_v2(dispatch, &actual, &fn) != 0) {
        cancel_restore_if_needed(restore_cancel_state, previous_cancel_state);
        return -1;
    }

    struct execution_cleanup cleanup = {
        .width = actual,
        .previous_cancel_state = previous_cancel_state,
        .restore_cancel_state = restore_cancel_state,
    };
    pthread_cleanup_push(execution_cleanup_handler, &cleanup);
    callback_rc = fn(dispatch->variants.context, offset, work_items);
    if (callback_rc == 0) {
        account_completed_work(work_items);
        if (used_width) *used_width = actual;
    }
    pthread_cleanup_pop(1);
    return callback_rc;
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
        if (rc != 0) return rc;
        offset += chunk;
        remaining -= chunk;
    }
    if (last_used_width) *last_used_width = last;
    return 0;
}
