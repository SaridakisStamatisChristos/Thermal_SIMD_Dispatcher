#include <thermal/simd/runtime.h>

#include <stdlib.h>
#include <string.h>

#include <config/runtime_flags.h>
#include <observability/telemetry_state.h>
#include <thermal/simd/telemetry_fusion.h>

static int software_upgrades_explicitly_allowed(void) {
    const char *value = getenv("TSD_ALLOW_SOFTWARE_UPGRADES");
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

int tsd_runtime_width_authorized(simd_width_t width) {
    if (width < SIMD_SSE41 || width > SIMD_AVX512) {
        return 0;
    }
    if (width == SIMD_SSE41) {
        return 1;
    }
    if (!tsd_runtime_flags_allow_transitions()) {
        return 0;
    }

    /* No live adaptive runtime: the low-level selector still applies its own
     * host-ISA and static AVX-512 policy checks. */
    if (!tsd_observability_runtime_guard_active()) {
        return 1;
    }

    const int perf_mode = tsd_observability_perf_mode();
    if (perf_mode == TSD_PERF_MODE_HARDWARE) {
        if (!tsd_observability_perf_hardware_fresh()) {
            return 0;
        }
    } else if (perf_mode == TSD_PERF_MODE_SOFTWARE) {
        if (!software_upgrades_explicitly_allowed()) {
            return 0;
        }
    } else {
        return 0;
    }

    return tsd_telemetry_temperature_upgrade_allowed() != 0;
}
