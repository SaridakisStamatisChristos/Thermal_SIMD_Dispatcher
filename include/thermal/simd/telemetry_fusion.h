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
 * Publish a normalized direct sample into the fusion bus. This gives platform
 * adapters a stable C boundary for contributing temperature/frequency data
 * without depending on the C++ collector implementation.
 */
int tsd_telemetry_fusion_publish_sample(const tsd_telemetry_sample_t *sample);

/*
 * Returns 0 only when at least one usable temperature/frequency signal is
 * available. Empty/degraded snapshots return -1 so callers can fall back to
 * their authoritative direct telemetry source.
 */
int tsd_telemetry_fusion_sample(tsd_telemetry_sample_t *out);

/*
 * Safety gate used by the immutable dispatcher. Before fusion starts this is
 * permissive for legacy startup compatibility. Once fusion is running, wider
 * SIMD selections require a currently usable package-temperature signal.
 */
int tsd_telemetry_temperature_upgrade_allowed(void);

#ifdef __cplusplus
}
#endif

#endif /* TSD_TELEMETRY_FUSION_H */
