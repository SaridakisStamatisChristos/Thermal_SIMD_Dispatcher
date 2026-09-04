#include <thermal/simd/runtime.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "runtime_guard_internal.h"

/* Private lifecycle emitted by src/thermal_simd.c under TSD_RUNTIME_INTERNAL_IMPL. */
struct tsd_runtime_impl;
int tsd_runtime_start_impl(struct tsd_runtime_impl **out_runtime, tsd_workload_fn workload);
void tsd_runtime_request_stop_impl(struct tsd_runtime_impl *runtime);
int tsd_runtime_stop_impl(struct tsd_runtime_impl *runtime);
int tsd_runtime_is_running_impl(const struct tsd_runtime_impl *runtime);
tsd_perf_mode_t tsd_runtime_perf_mode_impl(const struct tsd_runtime_impl *runtime);

struct tsd_runtime {
    struct tsd_runtime_impl *impl;
    uint64_t generation;
    int stopped;
    int stop_in_progress;
};

static pthread_mutex_t g_public_runtime_lock = PTHREAD_MUTEX_INITIALIZER;
static tsd_runtime_t *g_public_active_runtime = NULL;
static int g_public_runtime_starting = 0;
static uint64_t g_next_runtime_generation = 1;

int tsd_runtime_start_with_config(tsd_runtime_t **out_runtime,
                                  tsd_workload_fn workload,
                                  const tsd_runtime_config *config) {
    if (!out_runtime || !config) {
        errno = EINVAL;
        return -1;
    }
    *out_runtime = NULL;

    tsd_runtime_t *handle = calloc(1, sizeof(*handle));
    if (!handle) return -1;

    int lock_rc = pthread_mutex_lock(&g_public_runtime_lock);
    if (lock_rc != 0) {
        free(handle);
        errno = lock_rc;
        return -1;
    }
    if (g_public_active_runtime || g_public_runtime_starting ||
        tsd_runtime_config_snapshot_is_active()) {
        (void)pthread_mutex_unlock(&g_public_runtime_lock);
        free(handle);
        errno = EBUSY;
        return -1;
    }

    /* Reserve the process-wide start slot, then drop the public lifecycle lock
     * before baseline/probe work can invoke application code. A reentrant
     * runtime start observes this flag and returns EBUSY rather than deadlocking
     * on the public lifecycle mutex. */
    g_public_runtime_starting = 1;
    (void)pthread_mutex_unlock(&g_public_runtime_lock);

    if (tsd_runtime_config_activate_snapshot(config) != 0) {
        int saved_errno = errno;
        (void)pthread_mutex_lock(&g_public_runtime_lock);
        g_public_runtime_starting = 0;
        (void)pthread_mutex_unlock(&g_public_runtime_lock);
        free(handle);
        errno = saved_errno;
        return -1;
    }

    struct tsd_runtime_impl *impl = NULL;
    if (tsd_runtime_start_impl(&impl, workload) != 0) {
        int saved_errno = errno;
        tsd_runtime_config_deactivate_snapshot();
        (void)pthread_mutex_lock(&g_public_runtime_lock);
        g_public_runtime_starting = 0;
        (void)pthread_mutex_unlock(&g_public_runtime_lock);
        free(handle);
        errno = saved_errno;
        return -1;
    }

    lock_rc = pthread_mutex_lock(&g_public_runtime_lock);
    if (lock_rc != 0) {
        /* The private runtime is live; stop it before abandoning the public
         * handle so no orphaned generation survives a lifecycle-lock failure. */
        int stop_rc = tsd_runtime_stop_impl(impl);
        int saved_errno = stop_rc == 0 ? lock_rc : (errno ? errno : lock_rc);
        if (stop_rc == 0) tsd_runtime_config_deactivate_snapshot();
        free(handle);
        errno = saved_errno;
        return -1;
    }

    handle->impl = impl;
    handle->generation = g_next_runtime_generation++;
    if (g_next_runtime_generation == 0) g_next_runtime_generation = 1;
    handle->stopped = 0;
    handle->stop_in_progress = 0;
    g_public_active_runtime = handle;
    g_public_runtime_starting = 0;
    *out_runtime = handle;
    (void)pthread_mutex_unlock(&g_public_runtime_lock);
    return 0;
}

int tsd_runtime_start(tsd_runtime_t **out_runtime, tsd_workload_fn workload) {
    return tsd_runtime_start_with_config(out_runtime, workload, &g_tsd_config);
}

void tsd_runtime_request_stop(tsd_runtime_t *runtime) {
    if (!runtime) return;

    if (pthread_mutex_lock(&g_public_runtime_lock) != 0) return;
    if (runtime == g_public_active_runtime && !runtime->stopped &&
        !runtime->stop_in_progress && runtime->impl) {
        tsd_runtime_request_stop_impl(runtime->impl);
    }
    (void)pthread_mutex_unlock(&g_public_runtime_lock);
}

int tsd_runtime_stop(tsd_runtime_t *runtime) {
    if (!runtime) {
        errno = EINVAL;
        return -1;
    }

    int lock_rc = pthread_mutex_lock(&g_public_runtime_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        return -1;
    }
    if (runtime != g_public_active_runtime || runtime->stopped || !runtime->impl) {
        (void)pthread_mutex_unlock(&g_public_runtime_lock);
        errno = EINVAL;
        return -1;
    }
    if (runtime->stop_in_progress) {
        (void)pthread_mutex_unlock(&g_public_runtime_lock);
        errno = EBUSY;
        return -1;
    }

    /*
     * Do not hold the public lifecycle mutex while the private runtime drains
     * already-admitted application callbacks. During this interval the active
     * handle remains installed, so starts/destroys fail quickly with EBUSY,
     * while diagnostics fail closed without dereferencing the private runtime.
     */
    runtime->stop_in_progress = 1;
    struct tsd_runtime_impl *impl = runtime->impl;
    (void)pthread_mutex_unlock(&g_public_runtime_lock);

    int rc = tsd_runtime_stop_impl(impl);
    int saved_errno = errno;

    lock_rc = pthread_mutex_lock(&g_public_runtime_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        return -1;
    }
    if (rc == 0) {
        runtime->impl = NULL;
        runtime->stopped = 1;
        runtime->stop_in_progress = 0;
        g_public_active_runtime = NULL;
        tsd_runtime_config_deactivate_snapshot();
    } else {
        runtime->stop_in_progress = 0;
    }
    (void)pthread_mutex_unlock(&g_public_runtime_lock);

    if (rc != 0) errno = saved_errno;
    return rc;
}

int tsd_runtime_destroy(tsd_runtime_t *runtime) {
    if (!runtime) {
        errno = EINVAL;
        return -1;
    }

    int lock_rc = pthread_mutex_lock(&g_public_runtime_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        return -1;
    }
    if (runtime == g_public_active_runtime) {
        (void)pthread_mutex_unlock(&g_public_runtime_lock);
        errno = EBUSY;
        return -1;
    }
    if (runtime->stop_in_progress) {
        (void)pthread_mutex_unlock(&g_public_runtime_lock);
        errno = EBUSY;
        return -1;
    }
    if (!runtime->stopped || runtime->impl) {
        (void)pthread_mutex_unlock(&g_public_runtime_lock);
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_unlock(&g_public_runtime_lock);
    free(runtime);
    return 0;
}

int tsd_runtime_is_running(const tsd_runtime_t *runtime) {
    if (!runtime) return 0;

    if (pthread_mutex_lock(&g_public_runtime_lock) != 0) return 0;
    int running = runtime == g_public_active_runtime && !runtime->stopped &&
                  !runtime->stop_in_progress && runtime->impl &&
                  tsd_runtime_is_running_impl(runtime->impl);
    (void)pthread_mutex_unlock(&g_public_runtime_lock);
    return running;
}

tsd_perf_mode_t tsd_runtime_perf_mode(const tsd_runtime_t *runtime) {
    if (!runtime) return TSD_PERF_MODE_NONE;

    if (pthread_mutex_lock(&g_public_runtime_lock) != 0) return TSD_PERF_MODE_NONE;
    tsd_perf_mode_t mode = TSD_PERF_MODE_NONE;
    if (runtime == g_public_active_runtime && !runtime->stopped &&
        !runtime->stop_in_progress && runtime->impl) {
        mode = tsd_runtime_perf_mode_impl(runtime->impl);
    }
    (void)pthread_mutex_unlock(&g_public_runtime_lock);
    return mode;
}
