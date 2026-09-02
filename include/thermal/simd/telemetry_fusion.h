#ifndef TSD_TELEMETRY_FUSION_H
#define TSD_TELEMETRY_FUSION_H

#include <thermal/simd/telemetry_helper.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backward-compatible entry point: starts fusion on CPU 0. */
int tsd_telemetry_fusion_start(void);

/*
 * Starts (or acquires a reference to) the process-wide fusion service using
 * the supplied workload CPU for direct APERF/MPERF/cpufreq telemetry.
 * Re-acquiring for the same CPU increments the reference count. A concurrent
 * request for a different CPU fails with -1 rather than returning telemetry
 * from the wrong CPU; that caller should use its CPU-local direct helper.
 */
int tsd_telemetry_fusion_start_for_cpu(int cpu);

/* Releases one process-wide fusion reference; the last user stops the thread. */
void tsd_telemetry_fusion_stop(void);

/*
 * Publish a normalized direct sample into the fusion bus. Raw values are kept
 * on the safety channel before any configured EWMA is applied; filtered values
 * are exposed separately for forecasting/control.
 */
int tsd_telemetry_fusion_publish_sample(const tsd_telemetry_sample_t *sample);

/*
 * Returns 0 when at least one usable raw or filtered temperature/frequency
 * signal is available. The raw fields are freshness-checked safety values;
 * filtered_* fields are the optional control/forecast view.
 */
int tsd_telemetry_fusion_sample(tsd_telemetry_sample_t *out);

/*
 * Safety gate used by the immutable dispatcher. Before fusion starts this is
 * permissive for legacy startup compatibility. Once fusion is running, wider
 * SIMD selections require a fresh RAW package-temperature signal.
 */
int tsd_telemetry_temperature_upgrade_allowed(void);

#ifdef TSD_ENABLE_TESTS
/* Deterministic bridge tests can suppress host-specific direct sysfs/MSR data. */
void tsd_telemetry_fusion_test_disable_direct_helper(int disabled);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TSD_TELEMETRY_FUSION_H */
