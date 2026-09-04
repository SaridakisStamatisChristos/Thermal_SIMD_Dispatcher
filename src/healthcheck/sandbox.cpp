#include <healthcheck/sandbox.h>

#include <cerrno>
#include <cstdio>
#include <exception>
#include <new>
#include <sstream>

#include <thermal/simd/logging.h>
#include <thermal/simd/metrics.h>
#include <thermal/simd/telemetry_helper.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_perf.h>
#include <thermal/simd/thermal_trampoline.h>

namespace healthcheck {

namespace {

constexpr const char *kLogComponent = "sandbox";

} // namespace

SandboxResult RunSandbox() {
    SandboxResult result{};
    result.success = true;

    std::ostringstream diag;

    char reason[128];
    if (tsd_trampoline_self_validate(reason, sizeof(reason)) != 0) {
        result.patch_ok = false;
        result.success = false;
        diag << "trampoline validation failed";
        if (reason[0] != '\0') diag << ": " << reason;
    } else {
        result.patch_ok = true;
    }

    perf_ctx_t *ctx = tsd_perf_init(nullptr);
    if (!ctx) {
        result.success = false;
        result.telemetry_ok = false;
        if (!diag.str().empty()) diag << "; ";
        diag << "perf subsystem unavailable";
    } else {
        result.telemetry_ok = true;
        tsd_perf_mode_t mode = tsd_perf_get_mode(ctx);
        if (mode != TSD_PERF_MODE_HARDWARE) {
            result.success = false;
            result.telemetry_ok = false;
            if (!diag.str().empty()) diag << "; ";
            diag << "expected hardware counters but running in "
                 << (mode == TSD_PERF_MODE_SOFTWARE ? "software" : "unknown")
                 << " mode";
        }

        tsd_telemetry_helper_t telemetry{};
        if (tsd_perf_get_monitor_cpu(ctx) < 0 ||
            tsd_telemetry_helper_init(&telemetry, tsd_perf_get_monitor_cpu(ctx)) != 0) {
            result.success = false;
            result.telemetry_ok = false;
            if (!diag.str().empty()) diag << "; ";
            diag << "telemetry helper init failed";
        } else {
            tsd_telemetry_sample_t sample{};
            if (tsd_telemetry_helper_sample(&telemetry, &sample) != 0) {
                result.success = false;
                result.telemetry_ok = false;
                if (!diag.str().empty()) diag << "; ";
                diag << "telemetry sampling failed";
            } else if (!sample.temp_available && !sample.freq_ratio_available) {
                result.success = false;
                result.telemetry_ok = false;
                if (!diag.str().empty()) diag << "; ";
                diag << "no telemetry sources available";
            }
            tsd_telemetry_helper_destroy(&telemetry);
        }
        tsd_perf_cleanup(ctx);
    }

    if (result.success) {
        result.message = "sandbox passed";
        tsd_log_info(kLogComponent, "startup sandbox succeeded");
    } else {
        result.message = diag.str();
        if (result.message.empty()) result.message = "sandbox failed";
        tsd_log_warn(kLogComponent, "startup sandbox reported: %s", result.message.c_str());
        tsd_metrics_increment(TSD_METRIC_HEALTH_CHECK_FAILURES);
    }

    return result;
}

} // namespace healthcheck

extern "C" int tsd_sandbox_run(char *message, size_t message_len) {
    try {
        auto result = healthcheck::RunSandbox();
        if (message && message_len > 0) {
            if (result.message.empty()) message[0] = '\0';
            else std::snprintf(message, message_len, "%s", result.message.c_str());
        }
        return result.success ? 0 : -1;
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
    } catch (const std::exception &ex) {
        errno = EIO;
        tsd_log_error("sandbox", "sandbox C ABI failure: %s", ex.what());
    } catch (...) {
        errno = EIO;
        tsd_log_error("sandbox", "sandbox C ABI failure: unknown exception");
    }
    if (message && message_len > 0) {
        std::snprintf(message, message_len, "%s", "sandbox internal failure");
    }
    return -1;
}
