#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <config/runtime_flags.h>
#include <observability/telemetry_state.h>
#include <thermal/simd/adaptive_dispatch.h>
#include <thermal/simd/runtime.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_cpu.h>
#include <thermal/simd/thermal_trampoline.h>

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
    usleep(5000);
    /* Guard publication takes the write side and therefore cannot complete
       while the non-preemptible kernel still holds the execution side. */
    assert(atomic_load_explicit(&kernel.revocation_done, memory_order_acquire) == 0);

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

int main(void) {
    test_execution_revocation_linearization();

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
