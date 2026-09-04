#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

def write(path, text):
    (ROOT / path).write_text(text, encoding="utf-8")

def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)

p = "src/thermal_simd.c"
s = read(p)
s = replace_once(
    s,
    """struct tsd_runtime {\n    perf_ctx_t *perf;\n    pthread_t monitor;\n    int monitor_started;\n};""",
    """struct tsd_runtime {\n    perf_ctx_t *perf;\n    pthread_t monitor;\n    int monitor_started;\n    int stop_in_progress;\n};""",
    "private runtime stop claim",
)
old_stop = """int tsd_runtime_stop(tsd_runtime_t *runtime) {\n    if (!runtime) {\n        errno = EINVAL;\n        return -1;\n    }\n    if (tsd_runtime_current_thread_in_wide_execution()) {\n        errno = EDEADLK;\n        return -1;\n    }\n\n    pthread_mutex_lock(&g_tsd_runtime_lock);\n    if (runtime != g_tsd_active_runtime) {\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        errno = EINVAL;\n        return -1;\n    }\n\n    if (tsd_runtime_safety_write_enter() != 0) {\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        return -1;\n    }\n    tsd_runtime_set_stopping_locked(1);\n    atomic_store_explicit(&g_tsd_running, 0, memory_order_release);\n    tsd_runtime_safety_write_leave();\n\n    if (runtime->monitor_started) {\n        pthread_join(runtime->monitor, NULL);\n        runtime->monitor_started = 0;\n    }\n\n    /* Admission was closed before the monitor stopped. Drain only invocations\n     * that were already admitted; new wide work cannot join this set. */\n    if (tsd_runtime_wait_for_wide_quiescence() != 0) {\n        int saved_errno = errno ? errno : EIO;\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        errno = saved_errno;\n        return -1;\n    }\n\n    /* Successful shutdown requires the conservative selection to be committed.\n     * If that transition fails, retain the active stopping runtime and its guard\n     * resources so the caller can retry stop; never report a false-success\n     * cleanup while a wide selector remains published. */\n    if (tsd_trampoline_state_current_width() != SIMD_SSE41 &&\n        tsd_trampoline_patch(SIMD_SSE41) != 0) {\n        int saved_errno = errno ? errno : EIO;\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        errno = saved_errno;\n        return -1;\n    }\n\n    tsd_perf_cleanup(runtime->perf);\n    runtime->perf = NULL;\n    g_tsd_active_runtime = NULL;\n    free(runtime);\n\n    if (tsd_runtime_safety_write_enter() == 0) {\n        tsd_runtime_set_owner_tid_locked(0);\n        tsd_runtime_set_stopping_locked(0);\n        tsd_runtime_config_reset_dynamic_state();\n        tsd_runtime_safety_write_leave();\n    }\n    pthread_mutex_unlock(&g_tsd_runtime_lock);\n    return 0;\n}"""
new_stop = """int tsd_runtime_stop(tsd_runtime_t *runtime) {\n    if (!runtime) {\n        errno = EINVAL;\n        return -1;\n    }\n    if (tsd_runtime_current_thread_in_wide_execution()) {\n        errno = EDEADLK;\n        return -1;\n    }\n\n    pthread_mutex_lock(&g_tsd_runtime_lock);\n    if (runtime != g_tsd_active_runtime) {\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        errno = EINVAL;\n        return -1;\n    }\n    if (runtime->stop_in_progress) {\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        errno = EBUSY;\n        return -1;\n    }\n    runtime->stop_in_progress = 1;\n\n    if (tsd_runtime_safety_write_enter() != 0) {\n        runtime->stop_in_progress = 0;\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        return -1;\n    }\n    tsd_runtime_set_stopping_locked(1);\n    atomic_store_explicit(&g_tsd_running, 0, memory_order_release);\n    tsd_runtime_safety_write_leave();\n\n    /* Keep the active runtime installed as a STOPPING tombstone, but never\n     * hold the lifecycle mutex while waiting for monitor/application code. */\n    pthread_mutex_unlock(&g_tsd_runtime_lock);\n\n    if (runtime->monitor_started) {\n        pthread_join(runtime->monitor, NULL);\n        runtime->monitor_started = 0;\n    }\n\n    /* Admission was closed before the monitor stopped. Drain only invocations\n     * that were already admitted; new wide work cannot join this set. */\n    if (tsd_runtime_wait_for_wide_quiescence() != 0) {\n        int saved_errno = errno ? errno : EIO;\n        pthread_mutex_lock(&g_tsd_runtime_lock);\n        if (runtime == g_tsd_active_runtime) runtime->stop_in_progress = 0;\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        errno = saved_errno;\n        return -1;\n    }\n\n    /* Successful shutdown requires the conservative selection to be committed.\n     * If that transition fails, retain the active stopping runtime and its guard\n     * resources so the caller can retry stop; never report a false-success\n     * cleanup while a wide selector remains published. */\n    if (tsd_trampoline_state_current_width() != SIMD_SSE41 &&\n        tsd_trampoline_patch(SIMD_SSE41) != 0) {\n        int saved_errno = errno ? errno : EIO;\n        pthread_mutex_lock(&g_tsd_runtime_lock);\n        if (runtime == g_tsd_active_runtime) runtime->stop_in_progress = 0;\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        errno = saved_errno;\n        return -1;\n    }\n\n    pthread_mutex_lock(&g_tsd_runtime_lock);\n    if (runtime != g_tsd_active_runtime || !runtime->stop_in_progress) {\n        pthread_mutex_unlock(&g_tsd_runtime_lock);\n        errno = EBUSY;\n        return -1;\n    }\n\n    tsd_perf_cleanup(runtime->perf);\n    runtime->perf = NULL;\n    g_tsd_active_runtime = NULL;\n    free(runtime);\n\n    if (tsd_runtime_safety_write_enter() == 0) {\n        tsd_runtime_set_owner_tid_locked(0);\n        tsd_runtime_set_stopping_locked(0);\n        tsd_runtime_config_reset_dynamic_state();\n        tsd_runtime_safety_write_leave();\n    }\n    pthread_mutex_unlock(&g_tsd_runtime_lock);\n    return 0;\n}"""
s = replace_once(s, old_stop, new_stop, "private stop quiescence")
write(p, s)

p = "tests/runtime/test_runtime_lifecycle.c"
s = read(p)
block = r'''
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

'''
s = replace_once(s, "int main(void) {\n", block + "int main(void) {\n", "stop quiescence regression block")
s = replace_once(
    s,
    """    test_owner_domain_fail_closed();\n\n    assert(setenv(\"TSD_FAKE_PERF\", \"1\", 1) == 0);""",
    """    test_owner_domain_fail_closed();\n    test_stop_quiescence_releases_lifecycle_locks();\n\n    assert(setenv(\"TSD_FAKE_PERF\", \"1\", 1) == 0);""",
    "stop quiescence regression call",
)
write(p, s)
print("stop quiescence hardening applied")
