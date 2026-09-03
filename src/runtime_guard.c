#include <thermal/simd/runtime.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

#include <config/runtime_flags.h>
#include <observability/telemetry_state.h>
#include <thermal/simd/telemetry_fusion.h>
#include <thermal/simd/thermal_config.h>

#include "runtime_guard_internal.h"

static pthread_rwlock_t g_tsd_runtime_safety_gate = PTHREAD_RWLOCK_INITIALIZER;
static _Atomic int g_tsd_runtime_stopping = 0;

int tsd_runtime_execution_enter(void) {
    int rc = pthread_rwlock_rdlock(&g_tsd_runtime_safety_gate);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

void tsd_runtime_execution_leave(void) {
    (void)pthread_rwlock_unlock(&g_tsd_runtime_safety_gate);
}

int tsd_runtime_safety_write_enter(void) {
    int rc = pthread_rwlock_wrlock(&g_tsd_runtime_safety_gate);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

void tsd_runtime_safety_write_leave(void) {
    (void)pthread_rwlock_unlock(&g_tsd_runtime_safety_gate);
}

void tsd_runtime_set_stopping_locked(int stopping) {
    atomic_store_explicit(&g_tsd_runtime_stopping, stopping ? 1 : 0, memory_order_release);
}

int tsd_runtime_is_stopping(void) {
    return atomic_load_explicit(&g_tsd_runtime_stopping, memory_order_acquire) != 0;
}

static int raw_temperature_upgrade_allowed(void) {
    if (!tsd_telemetry_temperature_upgrade_allowed()) return 0;

    int freshness_ms = g_tsd_config.telemetry_max_skew_ms;
    if (freshness_ms < 0) freshness_ms = 150;

    double raw_temp_c = 0.0;
    if (!tsd_observability_raw_temperature_c(&raw_temp_c, freshness_ms) || !isfinite(raw_temp_c)) return 0;

    if (g_tsd_config.predictive_temp_ceiling_c < 20 ||
        g_tsd_config.predictive_temp_ceiling_c > 125 ||
        g_tsd_config.predictive_safety_margin_c < 0) {
        return 1;
    }
    const double limit_c = (double)(g_tsd_config.predictive_temp_ceiling_c -
                                    g_tsd_config.predictive_safety_margin_c);
    return raw_temp_c <= limit_c;
}

int tsd_runtime_width_authorized(simd_width_t width) {
    if (width < SIMD_SSE41 || width > SIMD_AVX512) return 0;
    if (width == SIMD_SSE41) return 1;
    if (tsd_runtime_is_stopping()) return 0;
    if (!tsd_runtime_flags_allow_transitions()) return 0;

    /* No live adaptive runtime: the low-level selector still applies its own
     * host-ISA and static AVX-512 policy checks. */
    if (!tsd_observability_runtime_guard_active()) return 1;

    const int perf_mode = tsd_observability_perf_mode();
    if (perf_mode != TSD_PERF_MODE_HARDWARE || !tsd_observability_perf_hardware_fresh()) {
        /* Software perf is deliberately fail-closed. A process-global
         * ns/work-item estimator cannot safely compare heterogeneous kernel
         * domains, so it is never an authority for wider SIMD. */
        return 0;
    }

    return raw_temperature_upgrade_allowed();
}
