#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <config/runtime_flags.h>
#include <observability/telemetry_state.h>
#include <thermal/simd/adaptive_dispatch.h>
#include <thermal/simd/runtime.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_cpu.h>
#include <thermal/simd/thermal_trampoline.h>

#include "runtime_guard_internal.h"

typedef struct {
    tsd_runtime_t *runtime;
    _Atomic int done;
} query_ctx_t;

static void *query_runtime(void *arg) {
    query_ctx_t *ctx = (query_ctx_t *)arg;
    while (!atomic_load_explicit(&ctx->done, memory_order_acquire)) {
        (void)tsd_runtime_is_running(ctx->runtime);
        (void)tsd_runtime_perf_mode(ctx->runtime);
    }
    return NULL;
}

static void configure_runtime(void) {
    tsd_runtime_flags_init();
    tsd_runtime_config_set_defaults(&g_tsd_config);
    g_tsd_config.check_interval_us = 1000;
    g_tsd_config.min_dwell_ms = 1;
    g_tsd_config.cooldown_down_ms = 1;
    g_tsd_config.cooldown_up_ms = 1;
    assert(tsd_runtime_config_refresh_ticks(&g_tsd_config) == 0);
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cv;
    int entered;
    int release;
    _Atomic int revocation_done;
    _Atomic int calls;
} blocking_kernel_ctx_t;

static void blocking_kernel(void *opaque, size_t work_items) {
    (void)work_items;
    blocking_kernel_ctx_t *ctx = (blocking_kernel_ctx_t *)opaque;
    atomic_fetch_add_explicit(&ctx->calls, 1, memory_order_relaxed);
    pthread_mutex_lock(&ctx->mutex);
    ctx->entered = 1;
    pthread_cond_broadcast(&ctx->cv);
    while (!ctx->release) pthread_cond_wait(&ctx->cv, &ctx->mutex);
    pthread_mutex_unlock(&ctx->mutex);
}

typedef struct {
    tsd_kernel_dispatch_t *dispatch;
    simd_width_t used;
    int rc;
} execute_ctx_t;

static void *execute_blocking(void *opaque) {
    execute_ctx_t *ctx = (execute_ctx_t *)opaque;
    ctx->rc = tsd_kernel_dispatch_execute(ctx->dispatch, 1, &ctx->used);
    return NULL;
}

static void *revoke_temperature(void *opaque) {
    blocking_kernel_ctx_t *ctx = (blocking_kernel_ctx_t *)opaque;
    tsd_temperature_channels_t channels = {0};
    channels.raw_available = 1;
    channels.raw_package_temp_c = 120.0;
    channels.filtered_available = 1;
    channels.filtered_package_temp_c = 80.0;
    tsd_observability_update_temperature_channels(&channels);
    atomic_store_explicit(&ctx->revocation_done, 1, memory_order_release);
    return NULL;
}

static void publish_hardware_guard(double raw_temp_c) {
    tsd_perf_telemetry_t perf = {0};
    perf.mode = TSD_PERF_MODE_HARDWARE;
    perf.counters_healthy = 1;
    perf.pinned_cpu = 0;
    perf.monitor_cpu = 0;
    tsd_observability_update_perf(&perf);

    tsd_temperature_channels_t channels = {0};
    channels.raw_available = 1;
    channels.raw_package_temp_c = raw_temp_c;
    channels.filtered_available = 1;
    channels.filtered_package_temp_c = raw_temp_c;
    tsd_observability_update_temperature_channels(&channels);
}

static void clear_observability_guard(void) {
    tsd_perf_telemetry_t perf = {0};
    tsd_observability_update_perf(&perf);
    tsd_temperature_channels_t channels = {0};
    tsd_observability_update_temperature_channels(&channels);
}

static void test_execution_revocation_linearization(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    tsd_runtime_flags_init();
    tsd_runtime_flags_record_sandbox_success();
    assert(tsd_trampoline_init() == 0);
    assert(tsd_trampoline_patch(SIMD_SSE41) == 0);

    tsd_runtime_config probe = g_tsd_config;
    probe.allow_avx512 = 1;
    simd_width_t host_max = tsd_detect_max_simd(&probe);
    if (host_max < SIMD_AVX2) {
        clear_observability_guard();
        return;
    }

    publish_hardware_guard(60.0);
    assert(tsd_runtime_width_authorized(SIMD_AVX2) == 1);
    assert(tsd_trampoline_patch(SIMD_AVX2) == 0);

    blocking_kernel_ctx_t kernel = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
        .entered = 0,
        .release = 0,
    };
    atomic_init(&kernel.revocation_done, 0);
    atomic_init(&kernel.calls, 0);

    tsd_kernel_variants_t variants = {
        .sse41 = blocking_kernel,
        .avx2 = blocking_kernel,
        .avx512 = blocking_kernel,
        .context = &kernel,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);

    execute_ctx_t exec = {.dispatch = dispatch, .used = SIMD_SSE41, .rc = -1};
    pthread_t execution_thread;
    assert(pthread_create(&execution_thread, NULL, execute_blocking, &exec) == 0);

    pthread_mutex_lock(&kernel.mutex);
    while (!kernel.entered) pthread_cond_wait(&kernel.cv, &kernel.mutex);
    pthread_mutex_unlock(&kernel.mutex);

    pthread_t revoker;
    assert(pthread_create(&revoker, NULL, revoke_temperature, &kernel) == 0);
    for (int i = 0; i < 100 &&
                    !atomic_load_explicit(&kernel.revocation_done, memory_order_acquire); ++i) {
        usleep(1000);
    }
    /* Revocation closes admission immediately and must not wait for an already
       admitted non-preemptible callback to finish. */
    assert(atomic_load_explicit(&kernel.revocation_done, memory_order_acquire) == 1);
    simd_width_t during = SIMD_AVX2;
    assert(tsd_kernel_dispatch_resolve(dispatch, &during) == 0);
    assert(during == SIMD_SSE41);

    pthread_mutex_lock(&kernel.mutex);
    kernel.release = 1;
    pthread_cond_broadcast(&kernel.cv);
    pthread_mutex_unlock(&kernel.mutex);

    assert(pthread_join(execution_thread, NULL) == 0);
    assert(exec.rc == 0);
    assert(exec.used == SIMD_AVX2);
    assert(pthread_join(revoker, NULL) == 0);
    assert(atomic_load_explicit(&kernel.revocation_done, memory_order_acquire) == 1);
    assert(tsd_runtime_width_authorized(SIMD_AVX2) == 0);

    simd_width_t resolved = SIMD_AVX2;
    assert(tsd_kernel_dispatch_resolve(dispatch, &resolved) == 0);
    assert(resolved == SIMD_SSE41);
    tsd_kernel_dispatch_destroy(dispatch);
    assert(tsd_trampoline_patch(SIMD_SSE41) == 0);
    clear_observability_guard();
}


typedef struct {
    _Atomic int done;
    int patch_rc;
} reentrant_kernel_ctx_t;

static void reentrant_selector_kernel(void *opaque, size_t work_items) {
    (void)work_items;
    reentrant_kernel_ctx_t *ctx = (reentrant_kernel_ctx_t *)opaque;
    ctx->patch_rc = tsd_trampoline_patch(SIMD_SSE41);
    atomic_store_explicit(&ctx->done, 1, memory_order_release);
}

static void *execute_reentrant(void *opaque) {
    execute_ctx_t *ctx = (execute_ctx_t *)opaque;
    ctx->rc = tsd_kernel_dispatch_execute(ctx->dispatch, 1, &ctx->used);
    return NULL;
}

static void test_callback_selector_reentrancy(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    tsd_runtime_flags_init();
    tsd_runtime_flags_record_sandbox_success();
    publish_hardware_guard(60.0);
    assert(tsd_trampoline_init() == 0);

    tsd_runtime_config probe = g_tsd_config;
    probe.allow_avx512 = 1;
    if (tsd_detect_max_simd(&probe) < SIMD_AVX2) {
        clear_observability_guard();
        return;
    }
    assert(tsd_trampoline_patch(SIMD_AVX2) == 0);

    reentrant_kernel_ctx_t kernel = {.patch_rc = -1};
    atomic_init(&kernel.done, 0);
    tsd_kernel_variants_t variants = {
        .sse41 = reentrant_selector_kernel,
        .avx2 = reentrant_selector_kernel,
        .avx512 = reentrant_selector_kernel,
        .context = &kernel,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);

    execute_ctx_t exec = {.dispatch = dispatch, .used = SIMD_SSE41, .rc = -1};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, execute_reentrant, &exec) == 0);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    assert(pthread_timedjoin_np(thread, NULL, &deadline) == 0);
    assert(exec.rc == 0);
    assert(exec.used == SIMD_AVX2);
    assert(atomic_load_explicit(&kernel.done, memory_order_acquire) == 1);
    assert(kernel.patch_rc == 0);
    assert(tsd_trampoline_state_current_width() == SIMD_SSE41);
    tsd_kernel_dispatch_destroy(dispatch);
    clear_observability_guard();
}

typedef struct {
    tsd_kernel_dispatch_t *dispatch;
    _Atomic int stop;
} reader_loop_ctx_t;

static void no_op_kernel(void *opaque, size_t work_items) {
    (void)opaque;
    (void)work_items;
}

static void *dispatch_reader_loop(void *opaque) {
    reader_loop_ctx_t *ctx = (reader_loop_ctx_t *)opaque;
    while (!atomic_load_explicit(&ctx->stop, memory_order_acquire)) {
        simd_width_t used = SIMD_SSE41;
        assert(tsd_kernel_dispatch_execute(ctx->dispatch, 1, &used) == 0);
    }
    return NULL;
}

static void test_revocation_not_starved_by_readers(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    tsd_runtime_flags_init();
    tsd_runtime_flags_record_sandbox_success();
    publish_hardware_guard(60.0);
    assert(tsd_trampoline_init() == 0);

    tsd_runtime_config probe = g_tsd_config;
    probe.allow_avx512 = 1;
    if (tsd_detect_max_simd(&probe) < SIMD_AVX2) {
        clear_observability_guard();
        return;
    }
    assert(tsd_trampoline_patch(SIMD_AVX2) == 0);

    tsd_kernel_variants_t variants = {
        .sse41 = no_op_kernel,
        .avx2 = no_op_kernel,
        .avx512 = no_op_kernel,
        .context = NULL,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);
    reader_loop_ctx_t readers = {.dispatch = dispatch};
    atomic_init(&readers.stop, 0);
    pthread_t threads[8];
    for (size_t i = 0; i < 8; ++i) assert(pthread_create(&threads[i], NULL, dispatch_reader_loop, &readers) == 0);

    blocking_kernel_ctx_t revocation = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    atomic_init(&revocation.revocation_done, 0);
    pthread_t revoker;
    assert(pthread_create(&revoker, NULL, revoke_temperature, &revocation) == 0);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    assert(pthread_timedjoin_np(revoker, NULL, &deadline) == 0);
    assert(atomic_load_explicit(&revocation.revocation_done, memory_order_acquire) == 1);

    atomic_store_explicit(&readers.stop, 1, memory_order_release);
    for (size_t i = 0; i < 8; ++i) assert(pthread_join(threads[i], NULL) == 0);
    tsd_kernel_dispatch_destroy(dispatch);
    assert(tsd_trampoline_patch(SIMD_SSE41) == 0);
    clear_observability_guard();
}

typedef struct {
    tsd_kernel_dispatch_t *dispatch;
    simd_width_t resolved;
    int rc;
} resolve_thread_ctx_t;

static void *resolve_on_other_thread(void *opaque) {
    resolve_thread_ctx_t *ctx = (resolve_thread_ctx_t *)opaque;
    ctx->resolved = SIMD_AVX2;
    ctx->rc = tsd_kernel_dispatch_resolve(ctx->dispatch, &ctx->resolved);
    return NULL;
}

static void test_owner_domain_fail_closed(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    tsd_runtime_flags_init();
    tsd_runtime_flags_record_sandbox_success();
    publish_hardware_guard(60.0);
    assert(tsd_trampoline_init() == 0);

    tsd_runtime_config probe = g_tsd_config;
    probe.allow_avx512 = 1;
    if (tsd_detect_max_simd(&probe) < SIMD_AVX2) {
        clear_observability_guard();
        return;
    }
    assert(tsd_trampoline_patch(SIMD_AVX2) == 0);
    assert(tsd_runtime_safety_write_enter() == 0);
    tsd_runtime_set_owner_tid_locked((int)syscall(SYS_gettid));
    tsd_runtime_safety_write_leave();

    tsd_kernel_variants_t variants = {
        .sse41 = no_op_kernel,
        .avx2 = no_op_kernel,
        .avx512 = no_op_kernel,
        .context = NULL,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);
    simd_width_t local = SIMD_SSE41;
    assert(tsd_kernel_dispatch_resolve(dispatch, &local) == 0);
    assert(local == SIMD_AVX2);

    resolve_thread_ctx_t other = {.dispatch = dispatch, .resolved = SIMD_AVX2, .rc = -1};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, resolve_on_other_thread, &other) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(other.rc == 0);
    assert(other.resolved == SIMD_SSE41);

    assert(tsd_runtime_safety_write_enter() == 0);
    tsd_runtime_set_owner_tid_locked(0);
    tsd_runtime_safety_write_leave();
    tsd_kernel_dispatch_destroy(dispatch);
    assert(tsd_trampoline_patch(SIMD_SSE41) == 0);
    clear_observability_guard();
}


typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cv;
    tsd_runtime_t *runtime;
    int runtime_ready;
    int callback_entered;
    int dispatch_rc;
    simd_width_t used;
    int nested_start_rc;
    int nested_start_errno;
    int stop_rc;
    int stop_errno;
} stop_quiescence_ctx_t;

static void stop_quiescence_kernel(void *opaque, size_t work_items) {
    (void)work_items;
    stop_quiescence_ctx_t *ctx = (stop_quiescence_ctx_t *)opaque;
    pthread_mutex_lock(&ctx->mutex);
    ctx->callback_entered = 1;
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mutex);

    for (int i = 0; i < 4000 && !tsd_runtime_is_stopping(); ++i) usleep(1000);
    assert(tsd_runtime_is_stopping());

    tsd_runtime_t *nested = NULL;
    errno = 0;
    ctx->nested_start_rc = tsd_runtime_start(&nested, NULL);
    ctx->nested_start_errno = errno;
    assert(nested == NULL);
}

static void *stop_quiescence_owner(void *opaque) {
    stop_quiescence_ctx_t *ctx = (stop_quiescence_ctx_t *)opaque;
    tsd_runtime_t *runtime = NULL;
    assert(tsd_runtime_start(&runtime, NULL) == 0);

    pthread_mutex_lock(&ctx->mutex);
    ctx->runtime = runtime;
    ctx->runtime_ready = 1;
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mutex);

    publish_hardware_guard(60.0);
    assert(tsd_trampoline_patch(SIMD_AVX2) == 0);

    tsd_kernel_variants_t variants = {
        .sse41 = stop_quiescence_kernel,
        .avx2 = stop_quiescence_kernel,
        .avx512 = stop_quiescence_kernel,
        .context = ctx,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    assert(tsd_kernel_dispatch_create(&variants, &dispatch) == 0);
    ctx->used = SIMD_SSE41;
    ctx->dispatch_rc = tsd_kernel_dispatch_execute(dispatch, 1, &ctx->used);
    tsd_kernel_dispatch_destroy(dispatch);
    return NULL;
}

static void *stop_quiescence_stopper(void *opaque) {
    stop_quiescence_ctx_t *ctx = (stop_quiescence_ctx_t *)opaque;
    errno = 0;
    ctx->stop_rc = tsd_runtime_stop(ctx->runtime);
    ctx->stop_errno = errno;
    return NULL;
}

static void test_stop_quiescence_releases_lifecycle_locks(void) {
    tsd_runtime_config probe;
    tsd_runtime_config_set_defaults(&probe);
    probe.allow_avx512 = 1;
    if (tsd_detect_max_simd(&probe) < SIMD_AVX2) return;

    assert(setenv("TSD_FAKE_PERF", "1", 1) == 0);
    configure_runtime();
    g_tsd_config.check_interval_us = 250000;
    (void)snprintf(g_tsd_config.telemetry_profile_path,
                   sizeof(g_tsd_config.telemetry_profile_path),
                   "%s", "test-unsupported-profile");
    assert(tsd_runtime_config_refresh_ticks(&g_tsd_config) == 0);
    tsd_runtime_flags_record_sandbox_success();

    stop_quiescence_ctx_t ctx = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
        .nested_start_rc = 0,
        .nested_start_errno = 0,
        .stop_rc = -1,
    };

    pthread_t owner;
    assert(pthread_create(&owner, NULL, stop_quiescence_owner, &ctx) == 0);

    pthread_mutex_lock(&ctx.mutex);
    while (!ctx.runtime_ready || !ctx.callback_entered) {
        pthread_cond_wait(&ctx.cv, &ctx.mutex);
    }
    pthread_mutex_unlock(&ctx.mutex);

    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, stop_quiescence_stopper, &ctx) == 0);

    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 4;
    assert(pthread_timedjoin_np(stopper, NULL, &deadline) == 0);
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 4;
    assert(pthread_timedjoin_np(owner, NULL, &deadline) == 0);

    assert(ctx.stop_rc == 0);
    assert(ctx.dispatch_rc == 0);
    assert(ctx.used == SIMD_AVX2);
    assert(ctx.nested_start_rc != 0);
    assert(ctx.nested_start_errno == EBUSY);
    assert(tsd_runtime_destroy(ctx.runtime) == 0);

    clear_observability_guard();
    unsetenv("TSD_FAKE_PERF");
}

static void test_stop_failures_are_retryable(void) {
    assert(setenv("TSD_FAKE_PERF", "1", 1) == 0);

    configure_runtime();
    tsd_runtime_t *runtime = NULL;
    assert(tsd_runtime_start(&runtime, NULL) == 0);
    tsd_runtime_request_stop(runtime);
    tsd_test_force_stop_join_error(EINVAL);
    errno = 0;
    assert(tsd_runtime_stop(runtime) != 0);
    assert(errno == EINVAL);
    errno = 0;
    assert(tsd_runtime_destroy(runtime) != 0);
    assert(errno == EBUSY);
    assert(tsd_runtime_stop(runtime) == 0);
    assert(tsd_runtime_destroy(runtime) == 0);

    configure_runtime();
    runtime = NULL;
    assert(tsd_runtime_start(&runtime, NULL) == 0);
    tsd_runtime_request_stop(runtime);
    tsd_test_force_stop_final_guard_error(EAGAIN);
    errno = 0;
    assert(tsd_runtime_stop(runtime) != 0);
    assert(errno == EAGAIN);
    assert(tsd_runtime_perf_mode(runtime) == TSD_PERF_MODE_NONE);
    errno = 0;
    assert(tsd_runtime_destroy(runtime) != 0);
    assert(errno == EBUSY);
    assert(tsd_runtime_stop(runtime) == 0);
    assert(tsd_runtime_destroy(runtime) == 0);

    unsetenv("TSD_FAKE_PERF");
}

int main(void) {
    test_execution_revocation_linearization();
    test_callback_selector_reentrancy();
    test_revocation_not_starved_by_readers();
    test_owner_domain_fail_closed();
    test_stop_quiescence_releases_lifecycle_locks();
    test_stop_failures_are_retryable();

    assert(setenv("TSD_FAKE_PERF", "1", 1) == 0);
    configure_runtime();

    tsd_runtime_t *runtime = NULL;
    assert(tsd_runtime_start(&runtime, NULL) == 0);
    assert(runtime != NULL);
    assert(tsd_runtime_is_running(runtime));
    assert(tsd_runtime_perf_mode(runtime) == TSD_PERF_MODE_SOFTWARE);
    assert(atomic_load_explicit(&g_tsd_current_width, memory_order_acquire) == SIMD_SSE41);

    tsd_runtime_flags_record_sandbox_success();
    errno = 0;
    assert(tsd_trampoline_patch(SIMD_AVX2) != 0);
    assert(errno == EAGAIN);

    /* The old software-upgrade escape hatch is intentionally retired: a
       process-global ns/work-item estimator is not a safe authority across
       heterogeneous registered kernels. */
    errno = 0;
    assert(tsd_trampoline_patch(SIMD_AVX2) != 0);
    assert(errno == EAGAIN);
    assert(atomic_load_explicit(&g_tsd_current_width, memory_order_acquire) == SIMD_SSE41);

    tsd_runtime_t *second = NULL;
    errno = 0;
    assert(tsd_runtime_start(&second, NULL) != 0);
    assert(errno == EBUSY);
    assert(second == NULL);

    query_ctx_t query = {.runtime = runtime};
    atomic_init(&query.done, 0);
    pthread_t query_thread;
    assert(pthread_create(&query_thread, NULL, query_runtime, &query) == 0);

    usleep(5000);
    tsd_runtime_request_stop(runtime);
    /* STOPPING is itself a hard authorization input. */
    publish_hardware_guard(60.0);
    errno = 0;
    assert(tsd_trampoline_patch(SIMD_AVX2) != 0);
    assert(errno == EAGAIN);

    assert(tsd_runtime_stop(runtime) == 0);
    assert(atomic_load_explicit(&g_tsd_current_width, memory_order_acquire) == SIMD_SSE41);
    assert(!tsd_runtime_is_running(runtime));
    assert(tsd_runtime_perf_mode(runtime) == TSD_PERF_MODE_NONE);

    atomic_store_explicit(&query.done, 1, memory_order_release);
    assert(pthread_join(query_thread, NULL) == 0);

    /* A stopped tombstone cannot interfere with a later runtime generation. */
    configure_runtime();
    (void)snprintf(g_tsd_config.telemetry_profile_path,
                   sizeof(g_tsd_config.telemetry_profile_path),
                   "%s", "test-unsupported-profile");
    assert(tsd_runtime_start(&second, NULL) == 0);
    assert(second != NULL);
    assert(tsd_runtime_is_running(second));
    tsd_runtime_flags_record_sandbox_success();

    tsd_runtime_request_stop(runtime);
    usleep(2000);
    assert(tsd_runtime_is_running(second));

    errno = 0;
    assert(tsd_runtime_destroy(second) != 0);
    assert(errno == EBUSY);

    tsd_runtime_request_stop(second);
    assert(tsd_runtime_stop(second) == 0);
    assert(tsd_runtime_destroy(second) == 0);
    assert(tsd_runtime_destroy(runtime) == 0);

    clear_observability_guard();
    unsetenv("TSD_FAKE_PERF");
    return 0;
}
