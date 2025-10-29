#include <thermal/simd/telemetry_fusion.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>

#include <telemetry/bus.h>
#include <telemetry/collector.h>
#include <telemetry/fusion.h>

namespace {

std::mutex g_fusion_mutex;
std::unique_ptr<telemetry::TelemetryFusion> g_fusion;

telemetry::TelemetryFusionConfig default_config() {
    telemetry::TelemetryFusionConfig config;
    config.poll_interval = std::chrono::milliseconds(50);
    config.freshness_window = std::chrono::milliseconds(150);
    config.ring_size = 128;
    return config;
}

}  // namespace

extern "C" int tsd_telemetry_fusion_start(void) {
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    if (g_fusion && g_fusion->running()) {
        return 0;
    }

    auto manager = std::make_shared<telemetry::TelemetryBusManager>();
    telemetry::TelemetryFusionConfig config = default_config();
    g_fusion = std::make_unique<telemetry::TelemetryFusion>(config, manager);
    g_fusion->start();
    return 0;
}

extern "C" void tsd_telemetry_fusion_stop(void) {
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    if (!g_fusion) {
        return;
    }
    g_fusion->stop();
    g_fusion.reset();
}

extern "C" int tsd_telemetry_fusion_sample(tsd_telemetry_sample_t *out) {
    if (!out) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    if (!g_fusion) {
        return -1;
    }
    auto snapshot = g_fusion->latest_snapshot();
    if (!snapshot) {
        return -1;
    }
    out->temp_available = snapshot->temp_available ? 1 : 0;
    out->freq_ratio_available = snapshot->freq_available ? 1 : 0;
    out->package_temp_millic = snapshot->temp_available
                                   ? static_cast<int32_t>(std::llround(snapshot->package_temp_c * 1000.0))
                                   : 0;
    out->freq_ratio_milli = snapshot->freq_available
                                ? static_cast<uint32_t>(std::llround(snapshot->freq_ratio))
                                : 0u;
    return 0;
}

