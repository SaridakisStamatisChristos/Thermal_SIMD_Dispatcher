#include <thermal/simd/health_check.h>

#include <stdio.h>
#include <string.h>

#include <thermal/simd/logging.h>
#include <thermal/simd/metrics.h>
#include <thermal/simd/telemetry_helper.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_perf.h>
#include <thermal/simd/thermal_trampoline.h>

#define LOG_COMPONENT "health"

int tsd_run_health_check(void) {
    int failures = 0;
    perf_ctx_t *ctx = tsd_perf_init(NULL);
    if (!ctx) {
        tsd_log_error(LOG_COMPONENT, "perf subsystem unavailable");
        tsd_metrics_increment(TSD_METRIC_HEALTH_CHECK_FAILURES);
        return 1;
    }
    tsd_perf_measure_baseline(ctx, &g_tsd_config);
    tsd_perf_mode_t mode = tsd_perf_get_mode(ctx);
    if (mode != TSD_PERF_MODE_HARDWARE) {
        tsd_log_error(LOG_COMPONENT, "expected hardware counters but running in %s mode",
                      mode == TSD_PERF_MODE_SOFTWARE ? "software" : "unknown");
        failures++;
    }
    tsd_thermal_eval_t eval = {0};
    if (tsd_perf_evaluate(ctx, &eval, &g_tsd_config) < 0) {
        tsd_log_error(LOG_COMPONENT, "failed to evaluate perf counters");
        failures++;
    }
    char reason[128];
    if (tsd_trampoline_self_validate(reason, sizeof(reason)) != 0) {
        tsd_log_error(LOG_COMPONENT, "trampoline validation failed: %s", reason);
        failures++;
    }
    tsd_telemetry_helper_t telemetry;
    if (tsd_telemetry_helper_init(&telemetry, tsd_perf_get_monitor_cpu(ctx)) != 0) {
        tsd_log_error(LOG_COMPONENT, "failed to initialise telemetry helper");
        failures++;
    } else {
        tsd_telemetry_sample_t sample = {0};
        if (tsd_telemetry_helper_sample(&telemetry, &sample) != 0) {
            tsd_log_error(LOG_COMPONENT, "telemetry sampling failed");
            failures++;
        } else if (!sample.temp_available && !sample.freq_ratio_available) {
            tsd_log_error(LOG_COMPONENT, "no telemetry sources available");
            failures++;
        }
        tsd_telemetry_helper_destroy(&telemetry);
    }
    tsd_perf_cleanup(ctx);
    if (failures > 0) {
        tsd_metrics_increment(TSD_METRIC_HEALTH_CHECK_FAILURES);
    }
    return failures ? 1 : 0;
}
