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
 */
int tsd_kernel_dispatch_execute(tsd_kernel_dispatch_t *dispatch,
                                size_t work_items,
                                simd_width_t *used_width);

/* Resolve without executing; useful for diagnostics and tests. */
int tsd_kernel_dispatch_resolve(const tsd_kernel_dispatch_t *dispatch,
                                simd_width_t *resolved_width);

#ifdef __cplusplus
}
#endif

#endif /* TSD_ADAPTIVE_DISPATCH_H */
