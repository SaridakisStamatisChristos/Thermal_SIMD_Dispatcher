#pragma once

#include <thermal/simd/simd_width.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsd_controller_telemetry_s {
    int fallback_active;
    simd_width_t current_width;
    simd_width_t recommended_width;
    int issued_change;
} tsd_controller_telemetry_t;

typedef struct tsd_fusion_telemetry_s {
    int running;
    int degraded;
    int temp_available;
    double package_temp_c;
    int freq_available;
    double freq_ratio;
    int cpi_available;
    double thermal_cpi;
    int power_available;
    double power_budget_w;
} tsd_fusion_telemetry_t;

typedef struct tsd_perf_telemetry_s {
    int mode; /* 0=none, 1=hardware, 2=software/degraded */
    int counters_healthy;
    int pinned_cpu;
    int monitor_cpu;
} tsd_perf_telemetry_t;

void tsd_observability_update_controller(const tsd_controller_telemetry_t *telemetry);
void tsd_observability_update_fusion(const tsd_fusion_telemetry_t *telemetry);
void tsd_observability_update_perf(const tsd_perf_telemetry_t *telemetry);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <chrono>
#include <mutex>

namespace observability {

struct ControllerTelemetrySnapshot {
    bool fallback_active{false};
    simd_width_t current_width{SIMD_SSE41};
    simd_width_t recommended_width{SIMD_SSE41};
    bool issued_change{false};
    std::chrono::system_clock::time_point updated_at{std::chrono::system_clock::time_point{}};
    std::chrono::steady_clock::time_point freshness_at{std::chrono::steady_clock::time_point{}};
};

struct FusionTelemetrySnapshot {
    bool running{false};
    bool degraded{false};
    bool temp_available{false};
    double package_temp_c{0.0};
    bool freq_available{false};
    double freq_ratio{0.0};
    bool cpi_available{false};
    double thermal_cpi{0.0};
    bool power_available{false};
    double power_budget_w{0.0};
    std::chrono::system_clock::time_point updated_at{std::chrono::system_clock::time_point{}};
    std::chrono::steady_clock::time_point freshness_at{std::chrono::steady_clock::time_point{}};
};

struct PerfTelemetrySnapshot {
    int mode{0};
    bool counters_healthy{false};
    int pinned_cpu{-1};
    int monitor_cpu{-1};
    std::chrono::system_clock::time_point updated_at{std::chrono::system_clock::time_point{}};
    std::chrono::steady_clock::time_point freshness_at{std::chrono::steady_clock::time_point{}};
};

class TelemetryState {
public:
    static TelemetryState &instance();

    void update_controller(const tsd_controller_telemetry_t *telemetry);
    void update_fusion(const tsd_fusion_telemetry_t *telemetry);
    void update_perf(const tsd_perf_telemetry_t *telemetry);

    ControllerTelemetrySnapshot controller_snapshot() const;
    FusionTelemetrySnapshot fusion_snapshot() const;
    PerfTelemetrySnapshot perf_snapshot() const;

private:
    TelemetryState() = default;

    mutable std::mutex mutex_;
    ControllerTelemetrySnapshot controller_{};
    FusionTelemetrySnapshot fusion_{};
    PerfTelemetrySnapshot perf_{};
};

}  // namespace observability
#endif
