#include <thermal/simd/runtime.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

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
};

static pthread_mutex_t g_public_runtime_lock = PTHREAD_MUTEX_INITIALIZER;
static tsd_runtime_t *g_public_active_runtime = NULL;
static uint64_t g_next_runtime_generation = 1;

int tsd_runtime_start(tsd_runtime_t **out_runtime, tsd_workload_fn workload) {
    if (!out_runtime) {
        errno = EINVAL;
        return -1;
    }
    *out_runtime = NULL;

    tsd_runtime_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return -1;
    }

    pthread_mutex_lock(&g_public_runtime_lock);
    if (g_public_active_runtime) {
        pthread_mutex_unlock(&g_public_runtime_lock);
        free(handle);
        errno = EBUSY;
        return -1;
    }

    struct tsd_runtime_impl *impl = NULL;
    if (tsd_runtime_start_impl(&impl, workload) != 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&g_public_runtime_lock);
        free(handle);
        errno = saved_errno;
        return -1;
    }

    handle->impl = impl;
    handle->generation = g_next_runtime_generation++;
    if (g_next_runtime_generation == 0) {
        g_next_runtime_generation = 1;
    }
    handle->stopped = 0;
    g_public_active_runtime = handle;
    *out_runtime = handle;
    pthread_mutex_unlock(&g_public_runtime_lock);
    return 0;
}

void tsd_runtime_request_stop(tsd_runtime_t *runtime) {
    if (!runtime) {
        return;
    }

    pthread_mutex_lock(&g_public_runtime_lock);
    if (runtime == g_public_active_runtime && !runtime->stopped && runtime->impl) {
        tsd_runtime_request_stop_impl(runtime->impl);
    }
    pthread_mutex_unlock(&g_public_runtime_lock);
}

int tsd_runtime_stop(tsd_runtime_t *runtime) {
    if (!runtime) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_public_runtime_lock);
    if (runtime != g_public_active_runtime || runtime->stopped || !runtime->impl) {
        pthread_mutex_unlock(&g_public_runtime_lock);
        errno = EINVAL;
        return -1;
    }

    int rc = tsd_runtime_stop_impl(runtime->impl);
    if (rc == 0) {
        runtime->impl = NULL;
        runtime->stopped = 1;
        g_public_active_runtime = NULL;
    }
    pthread_mutex_unlock(&g_public_runtime_lock);
    return rc;
}

int tsd_runtime_destroy(tsd_runtime_t *runtime) {
    if (!runtime) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_public_runtime_lock);
    if (runtime == g_public_active_runtime) {
        pthread_mutex_unlock(&g_public_runtime_lock);
        errno = EBUSY;
        return -1;
    }
    if (!runtime->stopped || runtime->impl) {
        pthread_mutex_unlock(&g_public_runtime_lock);
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_unlock(&g_public_runtime_lock);
    free(runtime);
    return 0;
}

int tsd_runtime_is_running(const tsd_runtime_t *runtime) {
    if (!runtime) {
        return 0;
    }

    pthread_mutex_lock(&g_public_runtime_lock);
    int running = runtime == g_public_active_runtime && !runtime->stopped && runtime->impl &&
                  tsd_runtime_is_running_impl(runtime->impl);
    pthread_mutex_unlock(&g_public_runtime_lock);
    return running;
}

tsd_perf_mode_t tsd_runtime_perf_mode(const tsd_runtime_t *runtime) {
    if (!runtime) {
        return TSD_PERF_MODE_NONE;
    }

    pthread_mutex_lock(&g_public_runtime_lock);
    tsd_perf_mode_t mode = TSD_PERF_MODE_NONE;
    if (runtime == g_public_active_runtime && !runtime->stopped && runtime->impl) {
        mode = tsd_runtime_perf_mode_impl(runtime->impl);
    }
    pthread_mutex_unlock(&g_public_runtime_lock);
    return mode;
}
