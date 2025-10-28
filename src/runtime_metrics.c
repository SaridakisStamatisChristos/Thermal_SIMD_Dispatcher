#include <thermal/simd/metrics.h>

#include <stdatomic.h>
#include <stddef.h>

static _Atomic uint64_t g_tsd_metric_counters[TSD_METRIC_COUNT];

void tsd_metrics_increment(tsd_metric_counter_t id) {
    if (id < 0 || id >= TSD_METRIC_COUNT) {
        return;
    }
    atomic_fetch_add_explicit(&g_tsd_metric_counters[id], 1, memory_order_relaxed);
}

void tsd_metrics_add(tsd_metric_counter_t id, uint64_t value) {
    if (id < 0 || id >= TSD_METRIC_COUNT) {
        return;
    }
    atomic_fetch_add_explicit(&g_tsd_metric_counters[id], value, memory_order_relaxed);
}

void tsd_metrics_snapshot(tsd_metrics_snapshot_t *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < TSD_METRIC_COUNT; ++i) {
        out->counters[i] = atomic_load_explicit(&g_tsd_metric_counters[i], memory_order_relaxed);
    }
}

const char* tsd_metrics_counter_name(tsd_metric_counter_t id) {
    switch (id) {
        case TSD_METRIC_PERF_FALLBACKS: return "perf_fallbacks";
        case TSD_METRIC_PERF_RECOVERIES: return "perf_recoveries";
        case TSD_METRIC_TELEMETRY_TEMP_DROPS: return "telemetry_temp_drops";
        case TSD_METRIC_TELEMETRY_TEMP_RECOVERIES: return "telemetry_temp_recoveries";
        case TSD_METRIC_TELEMETRY_FREQ_DROPS: return "telemetry_freq_drops";
        case TSD_METRIC_TELEMETRY_FREQ_RECOVERIES: return "telemetry_freq_recoveries";
        case TSD_METRIC_TELEMETRY_MSR_DROPS: return "telemetry_msr_drops";
        case TSD_METRIC_TELEMETRY_MSR_RECOVERIES: return "telemetry_msr_recoveries";
        case TSD_METRIC_PATCH_TRANSITIONS: return "patch_transitions";
        case TSD_METRIC_PATCH_FAILURES: return "patch_failures";
        case TSD_METRIC_HEALTH_CHECK_FAILURES: return "health_check_failures";
        case TSD_METRIC_SOFTWARE_TIMEOUT_ESCALATIONS: return "software_timeout_escalations";
        case TSD_METRIC_COUNT: return "invalid";
    }
    return "invalid";
}

void tsd_metrics_record_width_transition(simd_width_t from, simd_width_t to, int rc) {
    (void)from;
    (void)to;
    if (rc == 0) {
        tsd_metrics_increment(TSD_METRIC_PATCH_TRANSITIONS);
    } else {
        tsd_metrics_increment(TSD_METRIC_PATCH_FAILURES);
    }
}
