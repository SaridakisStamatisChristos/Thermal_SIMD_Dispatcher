#include <observability/telemetry_state.h>

#include <mutex>

namespace observability {

TelemetryState &TelemetryState::instance() {
    static TelemetryState state;
    return state;
}

void TelemetryState::update_controller(const tsd_controller_telemetry_t *telemetry) {
    if (!telemetry) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    controller_.fallback_active = telemetry->fallback_active != 0;
    controller_.current_width = telemetry->current_width;
    controller_.recommended_width = telemetry->recommended_width;
    controller_.issued_change = telemetry->issued_change != 0;
    controller_.updated_at = std::chrono::system_clock::now();
    controller_.freshness_at = std::chrono::steady_clock::now();
}

void TelemetryState::update_fusion(const tsd_fusion_telemetry_t *telemetry) {
    if (!telemetry) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    fusion_.running = telemetry->running != 0;
    fusion_.degraded = telemetry->degraded != 0;
    fusion_.temp_available = telemetry->temp_available != 0;
    fusion_.package_temp_c = telemetry->package_temp_c;
    fusion_.freq_available = telemetry->freq_available != 0;
    fusion_.freq_ratio = telemetry->freq_ratio;
    fusion_.cpi_available = telemetry->cpi_available != 0;
    fusion_.thermal_cpi = telemetry->thermal_cpi;
    fusion_.power_available = telemetry->power_available != 0;
    fusion_.power_budget_w = telemetry->power_budget_w;
    fusion_.updated_at = std::chrono::system_clock::now();
    fusion_.freshness_at = std::chrono::steady_clock::now();
}

void TelemetryState::update_perf(const tsd_perf_telemetry_t *telemetry) {
    if (!telemetry) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    perf_.mode = telemetry->mode;
    perf_.counters_healthy = telemetry->counters_healthy != 0;
    perf_.pinned_cpu = telemetry->pinned_cpu;
    perf_.monitor_cpu = telemetry->monitor_cpu;
    perf_.updated_at = std::chrono::system_clock::now();
    perf_.freshness_at = std::chrono::steady_clock::now();
}

ControllerTelemetrySnapshot TelemetryState::controller_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return controller_;
}

FusionTelemetrySnapshot TelemetryState::fusion_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fusion_;
}

PerfTelemetrySnapshot TelemetryState::perf_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return perf_;
}

}  // namespace observability

extern "C" {

void tsd_observability_update_controller(const tsd_controller_telemetry_t *telemetry) {
    observability::TelemetryState::instance().update_controller(telemetry);
}

void tsd_observability_update_fusion(const tsd_fusion_telemetry_t *telemetry) {
    observability::TelemetryState::instance().update_fusion(telemetry);
}

void tsd_observability_update_perf(const tsd_perf_telemetry_t *telemetry) {
    observability::TelemetryState::instance().update_perf(telemetry);
}

}  // extern "C"
