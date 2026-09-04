#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#include <config/runtime_flags.h>
#include <thermal/simd/runtime.h>
#include <thermal/simd/thermal_config.h>

#include "thermal_simd_test.h"

static void configure_runtime(void) {
    tsd_runtime_flags_init();
    tsd_runtime_config_set_defaults(&g_tsd_config);
    g_tsd_config.check_interval_us = 1000;
    g_tsd_config.min_dwell_ms = 1;
    g_tsd_config.cooldown_down_ms = 1;
    g_tsd_config.cooldown_up_ms = 1;
    assert(tsd_runtime_config_refresh_ticks(&g_tsd_config) == 0);
}

static void test_monitor_join_failure_is_retryable(void) {
    configure_runtime();

    tsd_runtime_t *runtime = NULL;
    assert(tsd_runtime_start(&runtime, NULL) == 0);
    assert(runtime != NULL);

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
}

static void test_final_guard_failure_is_retryable(void) {
    configure_runtime();

    tsd_runtime_t *runtime = NULL;
    assert(tsd_runtime_start(&runtime, NULL) == 0);
    assert(runtime != NULL);

    tsd_runtime_request_stop(runtime);
    tsd_test_force_stop_final_guard_error(EAGAIN);

    errno = 0;
    assert(tsd_runtime_stop(runtime) != 0);
    assert(errno == EAGAIN);

    /* Perf/telemetry cleanup may already be complete, but the public handle
     * must remain an active stopping tombstone until guard publication can be
     * completed on a later stop retry. */
    assert(tsd_runtime_perf_mode(runtime) == TSD_PERF_MODE_NONE);
    errno = 0;
    assert(tsd_runtime_destroy(runtime) != 0);
    assert(errno == EBUSY);

    assert(tsd_runtime_stop(runtime) == 0);
    assert(tsd_runtime_destroy(runtime) == 0);
}

int main(void) {
    assert(setenv("TSD_FAKE_PERF", "1", 1) == 0);
    test_monitor_join_failure_is_retryable();
    test_final_guard_failure_is_retryable();
    unsetenv("TSD_FAKE_PERF");
    return 0;
}
