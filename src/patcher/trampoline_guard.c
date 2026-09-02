#include <thermal/simd/thermal_trampoline.h>

#include <errno.h>

#include <thermal/simd/runtime.h>

/* Private selector emitted by trampoline.cpp under TSD_TRAMPOLINE_INTERNAL_IMPL. */
int tsd_trampoline_patch_impl(simd_width_t new_width);

int tsd_trampoline_patch(simd_width_t new_width) {
#ifdef TSD_ENABLE_TESTS
    return tsd_trampoline_patch_impl(new_width);
#else
    if (!tsd_runtime_width_authorized(new_width)) {
        errno = EAGAIN;
        return -1;
    }
    return tsd_trampoline_patch_impl(new_width);
#endif
}
