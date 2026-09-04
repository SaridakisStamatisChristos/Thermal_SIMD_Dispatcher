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

static int current_tid(void) {
    return (int)syscall(SYS_gettid);
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
        pthread_mutex_lock(&g_tsd_wide_drain_lock);
        pthread_cond_broadcast(&g_tsd_wide_drain_cv);
        pthread_mutex_unlock(&g_tsd_wide_drain_lock);
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
            pthread_mutex_unlock(&g_tsd_wide_drain_lock);
            errno = rc;
            return -1;
        }
    }
    pthread_mutex_unlock(&g_tsd_wide_drain_lock);
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
    if (stopping) tsd_runtime_wide_admission_close();
}

int tsd_runtime_is_stopping(void) {
    return atomic_load_explicit(&g_tsd_runtime_stopping, memory_order_acquire) != 0;
}

static int runtime_temperature_limits_valid(void) {
    return g_tsd_config.predictive_temp_ceiling_c >= 20 &&
           g_tsd_config.predictive_temp_ceiling_c <= 125 &&
           g_tsd_config.predictive_safety_margin_c >= 0 &&
           g_tsd_config.predictive_safety_margin_c <= 60 &&
           g_tsd_config.predictive_emergency_margin_c >= 0 &&
           g_tsd_config.predictive_emergency_margin_c <= 60;
}

static int raw_temperature_upgrade_allowed(void) {
    if (!tsd_telemetry_temperature_upgrade_allowed()) return 0;

    int freshness_ms = g_tsd_config.telemetry_max_skew_ms;
    if (freshness_ms < 0) freshness_ms = 150;

    double raw_temp_c = 0.0;
    if (!tsd_observability_raw_temperature_c(&raw_temp_c, freshness_ms) || !isfinite(raw_temp_c)) return 0;

    /* Safety configuration corruption is never an implicit opt-out. */
    if (!runtime_temperature_limits_valid()) return 0;

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
        /* Software perf is deliberately fail-closed. */
        return 0;
    }

    return raw_temperature_upgrade_allowed();
}
