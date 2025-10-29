#include <cstring>

#include <config/runtime_flags.h>
#include <healthcheck/sandbox.h>

extern "C" {
#include <thermal/simd/thermal_config.h>
}

int main() {
    tsd_runtime_config_set_defaults(&g_tsd_config);

    tsd_runtime_flags_init();
    if (tsd_runtime_flags_sandbox_complete() != 0) {
        return 1;
    }
    if (tsd_runtime_flags_allow_transitions() != 0) {
        return 1;
    }
    const char *initial = tsd_runtime_flags_status_message();
    if (!initial || std::strstr(initial, "sandbox") == nullptr) {
        return 1;
    }

    char diag[128] = {0};
    if (tsd_sandbox_run(diag, sizeof(diag)) == 0) {
        return 1;
    }
    if (diag[0] == '\0') {
        return 1;
    }

    tsd_runtime_flags_record_sandbox_failure(diag);
    if (tsd_runtime_flags_sandbox_complete() == 0) {
        return 1;
    }
    if (tsd_runtime_flags_allow_transitions() != 0) {
        return 1;
    }
    const char *failure = tsd_runtime_flags_status_message();
    if (!failure || std::strstr(failure, diag) == nullptr) {
        return 1;
    }

    tsd_runtime_flags_record_sandbox_success();
    if (tsd_runtime_flags_allow_transitions() == 0) {
        return 1;
    }
    const char *success = tsd_runtime_flags_status_message();
    if (!success || std::strstr(success, "passed") == nullptr) {
        return 1;
    }

    return 0;
}
