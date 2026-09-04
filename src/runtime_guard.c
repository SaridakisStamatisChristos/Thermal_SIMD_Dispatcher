#define _GNU_SOURCE
#include <thermal/simd/runtime.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <config/runtime_flags.h>
#include <observability/telemetry_state.h>
#include <thermal/simd/telemetry_fusion.h>
#include <thermal/simd/thermal_config.h>

#include "runtime_guard_internal.h"

/* Guard-state publication is short and never spans application code. */
static pthread_mutex_t g_tsd_guard_update_lock = PTHREAD_MUTEX_INITIALIZER;

/* Only shutdown/quiescence waits on this condition. Guard writers do not. */
static pthread_mutex_t g_tsd_wide_drain_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tsd_wide_drain_cv = PTHREAD_COND_INITIALIZER;

static _Atomic uint64_t g_tsd_active_wide_executions = 0;
static _Atomic int g_tsd_wide_admission_open = 0;
static _Atomic int g_tsd_runtime_stopping = 0;
static _Atomic int g_tsd_runtime_owner_tid = 0;
static _Thread_local unsigned int g_tsd_wide_execution_depth = 0;

/* 0=inactive, 1=publishing, 2=immutable active generation. */
static _Atomic int g_tsd_runtime_config_snapshot_state = 0;
static tsd_runtime_config g_tsd_runtime_config_snapshot;

typedef struct {
    int telemetry_max_skew_ms;
    int predictive_temp_ceiling_c;
    int predictive_safety_margin_c;
    int predictive_emergency_margin_c;
} tsd_safety_config_snapshot_t;

/* 0=uncaptured, 1=writer publishing, 2=immutable snapshot available. */
static _Atomic int g_tsd_safety_config_snapshot_state = 0;
static tsd_safety_config_snapshot_t g_tsd_safety_config_snapshot;

static int current_tid(void) {
    return (int)syscall(SYS_gettid);
}

static int validate_generation_config(tsd_runtime_config *cfg) {
    if (!cfg || cfg->check_interval_us <= 0 || cfg->down_count <= 0 || cfg->up_count <= 0 ||
        !isfinite(cfg->down_ratio) || cfg->down_ratio <= 0.0 ||
        cfg->cooldown_down_ms < 0 || cfg->cooldown_up_ms < 0 || cfg->min_dwell_ms < 0 ||
        cfg->memory_guard_divisor <= 0 || cfg->degraded_timeout_sec <= 0 ||
        cfg->telemetry_interval_ms < 10 || cfg->telemetry_interval_ms > 60000 ||
        cfg->telemetry_max_skew_ms < 0 || cfg->telemetry_max_skew_ms > 60000 ||
        !isfinite(cfg->telemetry_ewma_alpha) || cfg->telemetry_ewma_alpha < 0.0 ||
        cfg->telemetry_ewma_alpha > 1.0 ||
        cfg->predictive_temp_ceiling_c < 20 || cfg->predictive_temp_ceiling_c > 125 ||
        cfg->predictive_safety_margin_c < 0 || cfg->predictive_safety_margin_c > 60 ||
        cfg->predictive_emergency_margin_c < 0 || cfg->predictive_emergency_margin_c > 60 ||
        !isfinite(cfg->predictive_alpha) || cfg->predictive_alpha < 0.0 || cfg->predictive_alpha > 1.0) {
        errno = EINVAL;
        return -1;
    }
    if (cfg->down_ratio_milli == 0) {
        double scaled = cfg->down_ratio * 1000.0;
        if (!isfinite(scaled) || scaled < 1.0 || scaled > (double)UINT64_MAX) {
            errno = EINVAL;
            return -1;
        }
        cfg->down_ratio_milli = (uint64_t)(scaled + 0.5);
    }
    tsd_policy_config_apply_bounds(&cfg->policy);
    if (tsd_runtime_config_refresh_ticks(cfg) != 0) {
        if (errno == 0) errno = EINVAL;
        return -1;
    }
    return 0;
}

int tsd_runtime_config_activate_snapshot(const tsd_runtime_config *config) {
    if (!config) {
        errno = EINVAL;
        return -1;
    }

    tsd_runtime_config snapshot = *config;
    if (validate_generation_config(&snapshot) != 0) return -1;

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_tsd_runtime_config_snapshot_state,
                                                  &expected, 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }
    g_tsd_runtime_config_snapshot = snapshot;
    atomic_store_explicit(&g_tsd_runtime_config_snapshot_state, 2, memory_order_release);
    atomic_store_explicit(&g_tsd_safety_config_snapshot_state, 0, memory_order_release);
    return 0;
}

void tsd_runtime_config_deactivate_snapshot(void) {
    atomic_store_explicit(&g_tsd_safety_config_snapshot_state, 0, memory_order_release);
    atomic_store_explicit(&g_tsd_runtime_config_snapshot_state, 0, memory_order_release);
}

int tsd_runtime_config_snapshot_is_active(void) {
    return atomic_load_explicit(&g_tsd_runtime_config_snapshot_state, memory_order_acquire) == 2;
}

const tsd_runtime_config *tsd_runtime_config_active_snapshot(void) {
    if (atomic_load_explicit(&g_tsd_runtime_config_snapshot_state, memory_order_acquire) == 2) {
        return &g_tsd_runtime_config_snapshot;
    }
    return &g_tsd_config;
}

static int safety_config_snapshot(tsd_safety_config_snapshot_t *out) {
    if (!out) return 0;

    int state = atomic_load_explicit(&g_tsd_safety_config_snapshot_state, memory_order_acquire);
    if (state == 0) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&g_tsd_safety_config_snapshot_state,
                                                    &expected, 1,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            const tsd_runtime_config *runtime_cfg = tsd_runtime_config_active_snapshot();
            tsd_safety_config_snapshot_t snapshot = {
                .telemetry_max_skew_ms = runtime_cfg->telemetry_max_skew_ms,
                .predictive_temp_ceiling_c = runtime_cfg->predictive_temp_ceiling_c,
                .predictive_safety_margin_c = runtime_cfg->predictive_safety_margin_c,
                .predictive_emergency_margin_c = runtime_cfg->predictive_emergency_margin_c,
            };
            g_tsd_safety_config_snapshot = snapshot;
            atomic_store_explicit(&g_tsd_safety_config_snapshot_state, 2, memory_order_release);
            state = 2;
        } else {
            state = expected;
        }
    }

    /* Contention with the one-time publisher fails closed for this check. */
    if (state != 2) return 0;
    *out = g_tsd_safety_config_snapshot;
    return 1;
}

void tsd_runtime_wide_admission_close(void) {
    atomic_store_explicit(&g_tsd_wide_admission_open, 0, memory_order_release);
    /* Pair with the post-increment recheck in tsd_runtime_execution_enter(). */
    atomic_thread_fence(memory_order_seq_cst);
}

int tsd_runtime_wide_admission_is_open(void) {
    return atomic_load_explicit(&g_tsd_wide_admission_open, memory_order_acquire) != 0;
}

void tsd_runtime_set_owner_tid_locked(int tid) {
    atomic_store_explicit(&g_tsd_runtime_owner_tid, tid > 0 ? tid : 0, memory_order_release);
}

int tsd_runtime_current_thread_is_owner(void) {
    int owner = atomic_load_explicit(&g_tsd_runtime_owner_tid, memory_order_acquire);
    return owner <= 0 || current_tid() == owner;
}

int tsd_runtime_work_accounting_allowed(void) {
    /* Outside a live adaptive runtime there is no owner-domain restriction. */
    if (!tsd_observability_runtime_guard_active()) return 1;
    return tsd_runtime_current_thread_is_owner();
}

int tsd_runtime_current_thread_in_wide_execution(void) {
    return g_tsd_wide_execution_depth != 0;
}

static void wide_execution_release(void) {
    if (g_tsd_wide_execution_depth > 0) --g_tsd_wide_execution_depth;
    uint64_t previous = atomic_fetch_sub_explicit(&g_tsd_active_wide_executions, 1,
                                                  memory_order_acq_rel);
    if (previous == 0) {
        /* Defensive underflow repair: this is an internal invariant breach. */
        atomic_store_explicit(&g_tsd_active_wide_executions, 0, memory_order_release);
        return;
    }
    if (previous == 1) {
        int rc = pthread_mutex_lock(&g_tsd_wide_drain_lock);
        if (rc == 0) {
            (void)pthread_cond_broadcast(&g_tsd_wide_drain_cv);
            (void)pthread_mutex_unlock(&g_tsd_wide_drain_lock);
        }
    }
}

int tsd_runtime_execution_enter(simd_width_t width) {
    if (width < SIMD_SSE41 || width > SIMD_AVX512) {
        errno = EINVAL;
        return -1;
    }
    if (width == SIMD_SSE41) return 0;

    if (!tsd_runtime_current_thread_is_owner() ||
        !tsd_runtime_wide_admission_is_open() ||
        !tsd_runtime_width_authorized(width)) {
        errno = EAGAIN;
        return -1;
    }

    atomic_fetch_add_explicit(&g_tsd_active_wide_executions, 1, memory_order_acq_rel);
    ++g_tsd_wide_execution_depth;
    atomic_thread_fence(memory_order_seq_cst);

    /* A revoker that raced the first check closes admission before publishing
     * the new guard state. Back out instead of entering a stale wide target. */
    if (!tsd_runtime_wide_admission_is_open() ||
        !tsd_runtime_current_thread_is_owner() ||
        !tsd_runtime_width_authorized(width)) {
        wide_execution_release();
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

void tsd_runtime_execution_leave(simd_width_t width) {
    if (width > SIMD_SSE41 && width <= SIMD_AVX512) {
        wide_execution_release();
    }
}

int tsd_runtime_wait_for_wide_quiescence(void) {
    if (tsd_runtime_current_thread_in_wide_execution()) {
        errno = EDEADLK;
        return -1;
    }
    int rc = pthread_mutex_lock(&g_tsd_wide_drain_lock);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    while (atomic_load_explicit(&g_tsd_active_wide_executions, memory_order_acquire) != 0) {
        rc = pthread_cond_wait(&g_tsd_wide_drain_cv, &g_tsd_wide_drain_lock);
        if (rc != 0) {
            (void)pthread_mutex_unlock(&g_tsd_wide_drain_lock);
            errno = rc;
            return -1;
        }
    }
    rc = pthread_mutex_unlock(&g_tsd_wide_drain_lock);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

int tsd_runtime_safety_write_enter(void) {
    int rc = pthread_mutex_lock(&g_tsd_guard_update_lock);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    /* Close first. Existing wide kernels may finish; new ones cannot enter. */
    tsd_runtime_wide_admission_close();
    return 0;
}

void tsd_runtime_safety_write_leave(void) {
    int reopen = !tsd_runtime_is_stopping() && tsd_runtime_width_authorized(SIMD_AVX2);
    atomic_store_explicit(&g_tsd_wide_admission_open, reopen ? 1 : 0, memory_order_release);
    (void)pthread_mutex_unlock(&g_tsd_guard_update_lock);
}

void tsd_runtime_set_stopping_locked(int stopping) {
    atomic_store_explicit(&g_tsd_runtime_stopping, stopping ? 1 : 0, memory_order_release);
    if (stopping) {
        tsd_runtime_wide_admission_close();
        /* Each runtime/control generation captures a fresh immutable safety
         * configuration after it leaves the stopping/startup state. */
        atomic_store_explicit(&g_tsd_safety_config_snapshot_state, 0, memory_order_release);
    }
}

int tsd_runtime_is_stopping(void) {
    return atomic_load_explicit(&g_tsd_runtime_stopping, memory_order_acquire) != 0;
}

static int runtime_temperature_limits_valid(const tsd_safety_config_snapshot_t *cfg) {
    return cfg && cfg->predictive_temp_ceiling_c >= 20 &&
           cfg->predictive_temp_ceiling_c <= 125 &&
           cfg->predictive_safety_margin_c >= 0 &&
           cfg->predictive_safety_margin_c <= 60 &&
           cfg->predictive_emergency_margin_c >= 0 &&
           cfg->predictive_emergency_margin_c <= 60;
}

static int raw_temperature_upgrade_allowed(void) {
    if (!tsd_telemetry_temperature_upgrade_allowed()) return 0;

    tsd_safety_config_snapshot_t cfg;
    if (!safety_config_snapshot(&cfg) || !runtime_temperature_limits_valid(&cfg)) return 0;

    int freshness_ms = cfg.telemetry_max_skew_ms;
    if (freshness_ms < 0) freshness_ms = 150;

    double raw_temp_c = 0.0;
    if (!tsd_observability_raw_temperature_c(&raw_temp_c, freshness_ms) || !isfinite(raw_temp_c)) return 0;

    const double limit_c = (double)(cfg.predictive_temp_ceiling_c -
                                    cfg.predictive_safety_margin_c);
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
        /* Software perf is deliberately fail-closed. */
        return 0;
    }

    return raw_temperature_upgrade_allowed();
}
