#ifndef TSD_RUNTIME_METRICS_H
#define TSD_RUNTIME_METRICS_H

#include <stdint.h>

#include <thermal/simd/simd_width.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TSD_METRIC_PERF_FALLBACKS = 0,
    TSD_METRIC_PERF_RECOVERIES,
    TSD_METRIC_TELEMETRY_TEMP_DROPS,
    TSD_METRIC_TELEMETRY_TEMP_RECOVERIES,
    TSD_METRIC_TELEMETRY_FREQ_DROPS,
    TSD_METRIC_TELEMETRY_FREQ_RECOVERIES,
    TSD_METRIC_TELEMETRY_MSR_DROPS,
    TSD_METRIC_TELEMETRY_MSR_RECOVERIES,
    TSD_METRIC_PATCH_TRANSITIONS,
    TSD_METRIC_PATCH_FAILURES,
    TSD_METRIC_HEALTH_CHECK_FAILURES,
    TSD_METRIC_SOFTWARE_TIMEOUT_ESCALATIONS,
    TSD_METRIC_COUNT
} tsd_metric_counter_t;

typedef struct {
    uint64_t counters[TSD_METRIC_COUNT];
} tsd_metrics_snapshot_t;

void tsd_metrics_increment(tsd_metric_counter_t id);
void tsd_metrics_add(tsd_metric_counter_t id, uint64_t value);
void tsd_metrics_snapshot(tsd_metrics_snapshot_t *out);
const char* tsd_metrics_counter_name(tsd_metric_counter_t id);
void tsd_metrics_record_width_transition(simd_width_t from, simd_width_t to, int rc);

#ifdef __cplusplus
}
#endif

#endif /* TSD_RUNTIME_METRICS_H */
