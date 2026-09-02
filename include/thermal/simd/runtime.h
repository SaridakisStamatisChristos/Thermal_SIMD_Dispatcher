#ifndef TSD_RUNTIME_H
#define TSD_RUNTIME_H

#include <thermal/simd/thermal_perf.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Process-wide adaptive runtime lifecycle.
 *
 * The current implementation owns one process-global SIMD authorization state,
 * so only one runtime instance may be active at a time. Start the runtime from
 * the thread whose work should be measured: hardware perf events remain bound
 * to that owner TID across hot recovery. The runtime always begins at SSE4.1
 * and wider widths are authorized only by the live monitor after sandbox,
 * telemetry and perf validation.
 */
typedef struct tsd_runtime tsd_runtime_t;

/*
 * Starts the adaptive monitor. `workload` may be NULL for callers that account
 * application work through tsd_kernel_dispatch_execute[_chunked](). Returns
 * EBUSY when another process-global runtime is already active.
 */
int tsd_runtime_start(tsd_runtime_t **out_runtime, tsd_workload_fn workload);

/* Non-blocking cooperative stop request. Safe from ordinary application code. */
void tsd_runtime_request_stop(tsd_runtime_t *runtime);

/* Stops the monitor, restores perf-owner affinity and releases telemetry. */
int tsd_runtime_stop(tsd_runtime_t *runtime);

/* Lightweight diagnostics for embedders. */
int tsd_runtime_is_running(const tsd_runtime_t *runtime);
tsd_perf_mode_t tsd_runtime_perf_mode(const tsd_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif /* TSD_RUNTIME_H */
