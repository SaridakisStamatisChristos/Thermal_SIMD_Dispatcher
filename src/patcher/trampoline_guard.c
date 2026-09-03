#include <thermal/simd/thermal_trampoline.h>

#include <errno.h>
#include <pthread.h>

#include <thermal/simd/runtime.h>

/* Private selector emitted by trampoline.cpp under TSD_TRAMPOLINE_INTERNAL_IMPL. */
int tsd_trampoline_patch_impl(simd_width_t new_width);

#ifndef TSD_ENABLE_TESTS
/*
 * Serialize the production authorization decision with the complete immutable
 * selection operation. The implementation has its own executable-state lock;
 * this outer lock closes the check-then-select race between independent public
 * callers without coupling runtime-guard internals to the C++ patcher.
 */
static pthread_mutex_t g_tsd_authorized_patch_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

int tsd_trampoline_patch(simd_width_t new_width) {
#ifdef TSD_ENABLE_TESTS
    return tsd_trampoline_patch_impl(new_width);
#else
    int lock_rc = pthread_mutex_lock(&g_tsd_authorized_patch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        return -1;
    }

    if (!tsd_runtime_width_authorized(new_width)) {
        pthread_mutex_unlock(&g_tsd_authorized_patch_lock);
        errno = EAGAIN;
        return -1;
    }

    int rc = tsd_trampoline_patch_impl(new_width);
    int saved_errno = errno;

    /*
     * Guard state (perf/thermal freshness) is intentionally published outside
     * the patcher lock. Revalidate a successful wider selection before exposing
     * success to the caller. If authorization changed during selection, restore
     * the unconditional fail-closed SSE4.1 slot while the public transition
     * serializer is still held.
     */
    if (rc == 0 && new_width > SIMD_SSE41 && !tsd_runtime_width_authorized(new_width)) {
        int fallback_rc = tsd_trampoline_patch_impl(SIMD_SSE41);
        int fallback_errno = errno;
        rc = -1;
        saved_errno = fallback_rc == 0 ? EAGAIN : (fallback_errno != 0 ? fallback_errno : EIO);
    }

    pthread_mutex_unlock(&g_tsd_authorized_patch_lock);
    if (rc != 0) {
        errno = saved_errno != 0 ? saved_errno : EIO;
    }
    return rc;
#endif
}
