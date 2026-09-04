#include <observability/telemetry_state.h>

#include <chrono>
#include <mutex>

#include "../runtime_guard_internal.h"

namespace {

class SafetyUpdateGuard {
public:
    SafetyUpdateGuard() : locked_(tsd_runtime_safety_write_enter() == 0) {}
    ~SafetyUpdateGuard() {
        if (locked_) tsd_runtime_safety_write_leave();
    }
    explicit operator bool() const { return locked_; }
private:
    bool locked_;
};

}  // namespace

namespace observability {

TelemetryState &TelemetryState::instance() {
    static TelemetryState state;
    return state;
}

void TelemetryState::update_controller(const tsd_controller_telemetry_t *telemetry) {
    if (!telemetry) return;
    std::lock_guard<std::mutex> lock(mutex_);
    controller_.fallback_active = telemetry->fallback_active != 0;
    controller_.current_width = telemetry->current_width;
    controller_.recommended_width = telemetry->recommended_width;
    controller_.issued_change = telemetry->issued_change != 0;
    controller_.updated_at = std::chrono::system_clock::now();
    controller_.freshness_at = std::chrono::steady_clock::now();
}

void TelemetryState::update_fusion(const tsd_fusion_telemetry_t *telemetry) {
    if (!telemetry) return;
    SafetyUpdateGuard guard;
    if (!guard) return;
    std::lock_guard<std::mutex> lock(mutex_);
    fusion_.running = telemetry->running != 0;
    fusion_.degraded = telemetry->degraded != 0;
    fusion_.temp_available = telemetry->temp_available != 0;
    fusion_.package_temp_c = telemetry->package_temp_c;
    fusion_.filtered_temp_available = telemetry->temp_available != 0;
    fusion_.filtered_package_temp_c = telemetry->package_temp_c;
    fusion_.freq_available = telemetry->freq_available != 0;
    fusion_.freq_ratio = telemetry->freq_ratio;
    fusion_.cpi_available = telemetry->cpi_available != 0;
    fusion_.thermal_cpi = telemetry->thermal_cpi;
    fusion_.power_available = telemetry->power_available != 0;
    fusion_.power_budget_w = telemetry->power_budget_w;
    fusion_.updated_at = std::chrono::system_clock::now();
    fusion_.freshness_at = std::chrono::steady_clock::now();
}

void TelemetryState::update_temperature_channels(const tsd_temperature_channels_t *telemetry) {
    if (!telemetry) return;
    SafetyUpdateGuard guard;
    if (!guard) return;
    std::lock_guard<std::mutex> lock(mutex_);
    fusion_.raw_temp_available = telemetry->raw_available != 0;
    fusion_.raw_package_temp_c = telemetry->raw_package_temp_c;
    if (telemetry->raw_available) {
        fusion_.raw_temp_freshness_at = std::chrono::steady_clock::now();
    } else {
        fusion_.raw_temp_freshness_at = {};
    }
    fusion_.filtered_temp_available = telemetry->filtered_available != 0;
    fusion_.filtered_package_temp_c = telemetry->filtered_package_temp_c;
    if (telemetry->filtered_available) {
        fusion_.temp_available = true;
        fusion_.package_temp_c = telemetry->filtered_package_temp_c;
    } else if (!telemetry->raw_available) {
        fusion_.temp_available = false;
        fusion_.package_temp_c = 0.0;
    }
    fusion_.updated_at = std::chrono::system_clock::now();
    fusion_.freshness_at = std::chrono::steady_clock::now();
}

void TelemetryState::update_perf(const tsd_perf_telemetry_t *telemetry) {
    if (!telemetry) return;
    SafetyUpdateGuard guard;
    if (!guard) return;
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

bool TelemetryState::runtime_guard_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return perf_.mode != 0 || fusion_.running;
}

int TelemetryState::perf_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return perf_.mode;
}

bool TelemetryState::perf_hardware_fresh(std::chrono::seconds max_age) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (perf_.mode != 1 || !perf_.counters_healthy || perf_.freshness_at.time_since_epoch().count() == 0) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    return now >= perf_.freshness_at && (now - perf_.freshness_at) <= max_age;
}

bool TelemetryState::raw_temperature(double &out_temp_c, std::chrono::milliseconds max_age) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fusion_.raw_temp_available || fusion_.raw_temp_freshness_at.time_since_epoch().count() == 0) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < fusion_.raw_temp_freshness_at || (now - fusion_.raw_temp_freshness_at) > max_age) {
        return false;
    }
    out_temp_c = fusion_.raw_package_temp_c;
    return true;
}

}  // namespace observability

extern "C" {

void tsd_observability_update_controller(const tsd_controller_telemetry_t *telemetry) {
    try {
        observability::TelemetryState::instance().update_controller(telemetry);
    } catch (...) {
        /* Diagnostics are best-effort. */
    }
}

void tsd_observability_update_fusion(const tsd_fusion_telemetry_t *telemetry) {
    try {
        observability::TelemetryState::instance().update_fusion(telemetry);
    } catch (...) {
        tsd_runtime_wide_admission_close();
    }
}

void tsd_observability_update_temperature_channels(const tsd_temperature_channels_t *telemetry) {
    try {
        observability::TelemetryState::instance().update_temperature_channels(telemetry);
    } catch (...) {
        tsd_runtime_wide_admission_close();
    }
}

void tsd_observability_update_perf(const tsd_perf_telemetry_t *telemetry) {
    try {
        observability::TelemetryState::instance().update_perf(telemetry);
    } catch (...) {
        tsd_runtime_wide_admission_close();
    }
}

int tsd_observability_runtime_guard_active(void) {
    try {
        return observability::TelemetryState::instance().runtime_guard_active() ? 1 : 0;
    } catch (...) {
        return 1;
    }
}

int tsd_observability_perf_mode(void) {
    try {
        return observability::TelemetryState::instance().perf_mode();
    } catch (...) {
        return 0;
    }
}

int tsd_observability_perf_hardware_fresh(void) {
    try {
        return observability::TelemetryState::instance().perf_hardware_fresh(std::chrono::seconds(5)) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int tsd_observability_raw_temperature_c(double *out_temp_c, int max_age_ms) {
    if (!out_temp_c || max_age_ms < 0) return 0;
    try {
        double value = 0.0;
        if (!observability::TelemetryState::instance().raw_temperature(
                value, std::chrono::milliseconds(max_age_ms))) return 0;
        *out_temp_c = value;
        return 1;
    } catch (...) {
        return 0;
    }
}

}  // extern "C"
