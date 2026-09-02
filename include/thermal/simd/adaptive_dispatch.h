#ifndef TSD_ADAPTIVE_DISPATCH_H
#define TSD_ADAPTIVE_DISPATCH_H

#include <stddef.h>

#include <thermal/simd/simd_width.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Application-facing adaptive dispatch API.
 *
 * Variants are normal user-compiled functions; this API never rewrites their
 * code. The current thermal width is sampled atomically for each execution,
 * clamped to the host ISA, then resolved to the widest registered variant at
 * or below that width. SSE4.1 is mandatory because it is the runtime's
 * conservative baseline.
 */
typedef void (*tsd_kernel_fn)(void *context, size_t work_items);

typedef struct tsd_kernel_variants_s {
    tsd_kernel_fn sse41;
    tsd_kernel_fn avx2;
    tsd_kernel_fn avx512;
    void *context;
} tsd_kernel_variants_t;

typedef struct tsd_kernel_dispatch tsd_kernel_dispatch_t;

/* Returns 0 on success. The variant table is copied and immutable afterwards. */
int tsd_kernel_dispatch_create(const tsd_kernel_variants_t *variants,
                               tsd_kernel_dispatch_t **out_dispatch);

void tsd_kernel_dispatch_destroy(tsd_kernel_dispatch_t *dispatch);

/*
 * Resolves and invokes the active application variant. `used_width`, when
 * non-NULL, receives the implementation actually called after ISA/fallback
 * clamping. Successful work is also accounted into the software-perf workload
 * counter so degraded-mode adaptation remains meaningful for registered code.
 *
 * One call is one non-preemptible kernel invocation from the dispatcher's
 * perspective. Applications with long-running kernels should prefer
 * tsd_kernel_dispatch_execute_chunked() so thermal downgrade decisions can be
 * observed between bounded chunks.
 */
int tsd_kernel_dispatch_execute(tsd_kernel_dispatch_t *dispatch,
                                size_t work_items,
                                simd_width_t *used_width);

/*
 * Cooperatively executes `total_work_items` in chunks no larger than
 * `max_chunk_items`, re-resolving the currently authorized SIMD width before
 * every chunk. Kernels therefore need to interpret repeated calls as
 * successive units of work (typically by keeping any cursor/state in
 * `variants.context`). `last_used_width`, when non-NULL, receives the width of
 * the final chunk. A zero total performs no kernel call but still succeeds.
 */
int tsd_kernel_dispatch_execute_chunked(tsd_kernel_dispatch_t *dispatch,
                                        size_t total_work_items,
                                        size_t max_chunk_items,
                                        simd_width_t *last_used_width);

/* Resolve without executing; useful for diagnostics and tests. */
int tsd_kernel_dispatch_resolve(const tsd_kernel_dispatch_t *dispatch,
                                simd_width_t *resolved_width);

#ifdef __cplusplus
}
#endif

#endif /* TSD_ADAPTIVE_DISPATCH_H */
