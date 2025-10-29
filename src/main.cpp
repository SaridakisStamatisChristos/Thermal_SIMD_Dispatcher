#include <config/runtime_flags.h>
#include <healthcheck/sandbox.h>

#include <sched.h>
#include <thermal/simd/health_check.h>
#include <thermal/simd/logging.h>
#include <thermal/simd/metrics.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_cpu.h>
#include <thermal/simd/thermal_trampoline.h>
#include <thermal/simd/thermal_signals.h>

extern "C" int tsd_dispatcher_main(int argc, char **argv);

namespace {

constexpr const char *kLogComponent = "runtime";

} // namespace

int main(int argc, char **argv) {
    tsd_runtime_flags_init();

    tsd_log_info(kLogComponent, "=== Production Thermal-Aware SIMD Dispatcher ===");
    tsd_runtime_config_parse_cli(&g_tsd_config, argc, argv);
    tsd_log_set_level(g_tsd_config.log_level);

    if (tsd_runtime_flags_sandbox_only()) {
        if (tsd_trampoline_init() != 0) {
            tsd_log_error(kLogComponent, "Failed to create trampolines for sandbox");
            return 1;
        }
        if (tsd_trampoline_patch(SIMD_SSE41) != 0) {
            tsd_log_error(kLogComponent, "Failed to install baseline trampoline for sandbox");
            return 1;
        }
        char sandbox_diag[256];
        int sandbox_rc = tsd_sandbox_run(sandbox_diag, sizeof(sandbox_diag));
        if (sandbox_rc == 0) {
            tsd_runtime_flags_record_sandbox_success();
            return 0;
        }
        tsd_runtime_flags_record_sandbox_failure(sandbox_diag);
        tsd_log_error(kLogComponent, "Sandbox failed: %s", tsd_runtime_flags_status_message());
        return 1;
    }

    tsd_install_patch_signal_handlers();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    (void)sched_setaffinity(0, sizeof(cpuset), &cpuset);

    return tsd_dispatcher_main(argc, argv);
}
