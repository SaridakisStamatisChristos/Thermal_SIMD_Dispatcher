#include <thermal/simd/thermal_trampoline.h>

#include <errno.h>

#include <thermal/simd/runtime.h>

#include "../runtime_guard_internal.h"

/* Private selector emitted by trampoline.cpp under TSD_TRAMPOLINE_INTERNAL_IMPL. */
int tsd_trampoline_patch_impl(simd_width_t new_width);

int tsd_trampoline_patch(simd_width_t new_width) {
#ifdef TSD_ENABLE_TESTS
    /* White-box executable-memory tests deliberately bypass live guard state.
     * Production authorization is covered by tests linked to thermal_simd_core. */
    return tsd_trampoline_patch_impl(new_width);
#else
    /*
     * One write-side safety gate now covers both authorization and immutable
     * selection. Guard-state writers take the same gate, while application
     * execution holds its read side. Consequently no guard state can be
     * revoked between this check and publication, and no kernel invocation can
     * enter through a half-completed selection transition.
     */
    if (tsd_runtime_safety_write_enter() != 0) {
        return -1;
    }

    if (!tsd_runtime_width_authorized(new_width)) {
        tsd_runtime_safety_write_leave();
        errno = EAGAIN;
        return -1;
    }

    int rc = tsd_trampoline_patch_impl(new_width);
    int saved_errno = errno;
    tsd_runtime_safety_write_leave();

    if (rc != 0) {
        errno = saved_errno != 0 ? saved_errno : EIO;
    }
    return rc;
#endif
}
