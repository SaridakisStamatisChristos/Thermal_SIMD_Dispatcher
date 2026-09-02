#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <config/runtime_flags.h>
#include <thermal/simd/runtime.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_trampoline.h>

int main(void) {
    assert(setenv("TSD_FAKE_PERF", "1", 1) == 0);
    tsd_runtime_flags_init();
    tsd_runtime_config_set_defaults(&g_tsd_config);
    g_tsd_config.check_interval_us = 1000;
    g_tsd_config.min_dwell_ms = 1;
    g_tsd_config.cooldown_down_ms = 1;
    g_tsd_config.cooldown_up_ms = 1;
    assert(tsd_runtime_config_refresh_ticks(&g_tsd_config) == 0);

    tsd_runtime_t *runtime = NULL;
    assert(tsd_runtime_start(&runtime, NULL) == 0);
    assert(runtime != NULL);
    assert(tsd_runtime_is_running(runtime));
    assert(tsd_runtime_perf_mode(runtime) == TSD_PERF_MODE_SOFTWARE);
    assert(atomic_load_explicit(&g_tsd_current_width, memory_order_acquire) == SIMD_SSE41);

    tsd_runtime_t *second = NULL;
    errno = 0;
    assert(tsd_runtime_start(&second, NULL) != 0);
    assert(errno == EBUSY);
    assert(second == NULL);

    usleep(5000);
    tsd_runtime_request_stop(runtime);
    assert(tsd_runtime_stop(runtime) == 0);
    assert(atomic_load_explicit(&g_tsd_current_width, memory_order_acquire) == SIMD_SSE41);

    unsetenv("TSD_FAKE_PERF");
    return 0;
}
