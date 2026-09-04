#ifndef TSD_RUNTIME_H
#define TSD_RUNTIME_H

#include <thermal/simd/simd_width.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_perf.h>

/*
 * The implementation source is compiled with TSD_RUNTIME_INTERNAL_IMPL so its
 * legacy in-file lifecycle is emitted under private *_impl symbols and a
 * public lifetime-safe wrapper can own opaque handles without changing the
 * implementation source's monitor logic.
 */
#ifdef TSD_RUNTIME_INTERNAL_IMPL
#define tsd_runtime tsd_runtime_impl
#define tsd_runtime_start tsd_runtime_start_impl
#define tsd_runtime_request_stop tsd_runtime_request_stop_impl
#define tsd_runtime_stop tsd_runtime_stop_impl
#define tsd_runtime_is_running tsd_runtime_is_running_impl
#define tsd_runtime_perf_mode tsd_runtime_perf_mode_impl
#endif

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
 *
 * Public runtime handles remain valid after tsd_runtime_stop() so stale stopped
 * handles cannot alias a later runtime instance. Call tsd_runtime_destroy() once
 * no thread can reference a stopped handle. Using a handle after destroy is an
 * application error, just like using any other freed object.
 */
typedef struct tsd_runtime tsd_runtime_t;

/*
 * Optional baseline/probe callback contract:
 *
 * - the callback may be invoked repeatedly during startup calibration (the
 *   current implementation can invoke it up to 100,000 times per baseline);
 * - it should be bounded, nonblocking, and safe to repeat; avoid irreversible
 *   side effects;
 * - lifecycle re-entry is unsupported as useful work. A nested start is
 *   rejected with EBUSY instead of waiting on the outer startup;
 * - applications using registered dispatch accounting may pass NULL and let
 *   the runtime use its synthetic baseline probe.
 */

/*
 * Starts from the legacy g_tsd_config compatibility input. The configuration
 * is validated and copied into an immutable runtime-generation snapshot before
 * telemetry, perf, policy, or monitor threads start reading it.
 */
int tsd_runtime_start(tsd_runtime_t **out_runtime, tsd_workload_fn workload);

/*
 * Preferred embedding API. `config` is copied; the caller may reuse or mutate
 * its own object after this function returns without changing the live runtime.
 * Invalid configuration is rejected with EINVAL. Only one runtime generation
 * can be active or starting at a time.
 */
int tsd_runtime_start_with_config(tsd_runtime_t **out_runtime,
                                  tsd_workload_fn workload,
                                  const tsd_runtime_config *config);

/*
 * Non-blocking cooperative stop request. A NULL or stale stopped handle is a
 * no-op and can never stop a later runtime generation.
 */
void tsd_runtime_request_stop(tsd_runtime_t *runtime);

/*
 * Stops the monitor, restores perf-owner affinity and releases telemetry.
 * The handle becomes an inert tombstone and must later be released with
 * tsd_runtime_destroy().
 */
int tsd_runtime_stop(tsd_runtime_t *runtime);

/* Release a stopped handle. Returns EBUSY for the currently active runtime. */
int tsd_runtime_destroy(tsd_runtime_t *runtime);

/* Thread-safe diagnostics for embedders. */
int tsd_runtime_is_running(const tsd_runtime_t *runtime);
tsd_perf_mode_t tsd_runtime_perf_mode(const tsd_runtime_t *runtime);

/*
 * Returns non-zero when a width is currently eligible under live runtime
 * safety state. With no active runtime, this only applies static/runtime flags;
 * while a runtime is active, perf health and thermal authorization also gate
 * wider SIMD. SSE4.1 is always the fail-closed width.
 */
int tsd_runtime_width_authorized(simd_width_t width);

#ifdef __cplusplus
}
#endif

#endif /* TSD_RUNTIME_H */
