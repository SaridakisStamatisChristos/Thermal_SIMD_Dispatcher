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
#include <thermal/simd/runtime.h>
#include <thermal/simd/thermal_config.h>
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

int main(void) {
    assert(setenv("TSD_FAKE_PERF", "1", 1) == 0);
    unsetenv("TSD_ALLOW_SOFTWARE_UPGRADES");
    configure_runtime();

    tsd_runtime_t *runtime = NULL;
    assert(tsd_runtime_start(&runtime, NULL) == 0);
    assert(runtime != NULL);
    assert(tsd_runtime_is_running(runtime));
    assert(tsd_runtime_perf_mode(runtime) == TSD_PERF_MODE_SOFTWARE);
    assert(atomic_load_explicit(&g_tsd_current_width, memory_order_acquire) == SIMD_SSE41);

    /* The public compatibility selector must obey live degraded-mode policy. */
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
    assert(tsd_runtime_stop(runtime) == 0);
    assert(atomic_load_explicit(&g_tsd_current_width, memory_order_acquire) == SIMD_SSE41);
    assert(!tsd_runtime_is_running(runtime));
    assert(tsd_runtime_perf_mode(runtime) == TSD_PERF_MODE_NONE);

    atomic_store_explicit(&query.done, 1, memory_order_release);
    assert(pthread_join(query_thread, NULL) == 0);

    /* A stopped tombstone cannot interfere with a later runtime generation. */
    configure_runtime();
    /* Suppress process-wide fusion for this deterministic value-guard check;
       the perf context deliberately falls back to CPU-local direct telemetry. */
    (void)snprintf(g_tsd_config.telemetry_profile_path,
                   sizeof(g_tsd_config.telemetry_profile_path),
                   "%s", "test-unsupported-profile");
    assert(setenv("TSD_ALLOW_SOFTWARE_UPGRADES", "1", 1) == 0);
    assert(tsd_runtime_start(&second, NULL) == 0);
    assert(second != NULL);
    assert(tsd_runtime_is_running(second));

    tsd_temperature_channels_t channels = {0};
    channels.raw_available = 1;
    channels.raw_package_temp_c = 100.0;
    channels.filtered_available = 1;
    channels.filtered_package_temp_c = 70.0;
    tsd_observability_update_temperature_channels(&channels);
    assert(tsd_runtime_width_authorized(SIMD_AVX2) == 0);

    channels.raw_package_temp_c = 60.0;
    tsd_observability_update_temperature_channels(&channels);
    assert(tsd_runtime_width_authorized(SIMD_AVX2) == 1);

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

    unsetenv("TSD_ALLOW_SOFTWARE_UPGRADES");
    unsetenv("TSD_FAKE_PERF");
    return 0;
}
