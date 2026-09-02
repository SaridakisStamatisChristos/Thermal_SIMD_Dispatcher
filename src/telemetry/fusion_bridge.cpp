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
std::shared_ptr<telemetry::TelemetryBusManager> g_manager;
tsd_telemetry_helper_t g_direct_helper{};
bool g_direct_helper_ready = false;

telemetry::TelemetryFusionConfig default_config() {
    telemetry::TelemetryFusionConfig config;
    config.poll_interval = std::chrono::milliseconds(50);
    config.freshness_window = std::chrono::milliseconds(150);
    config.ring_size = 128;
    return config;
}

void publish_direct_sample(const tsd_telemetry_sample_t &sample) {
    if (!g_manager) {
        return;
    }
    auto bus = g_manager->bus();
    if (!bus) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (sample.temp_available) {
        telemetry::TelemetryReading reading;
        reading.value = static_cast<double>(sample.package_temp_millic) / 1000.0;
        reading.valid = true;
        reading.quality = 100;
        reading.timestamp = now;
        bus->publish(telemetry::TelemetrySignal::kPackageTempC, reading);
    }
    if (sample.freq_ratio_available) {
        telemetry::TelemetryReading reading;
        /* The fusion bus stores frequency ratio in milli-units end-to-end. */
        reading.value = static_cast<double>(sample.freq_ratio_milli);
        reading.valid = true;
        reading.quality = 100;
        reading.timestamp = now;
        bus->publish(telemetry::TelemetrySignal::kFrequencyRatio, reading);
    }
}

bool copy_usable_snapshot(const telemetry::TelemetrySnapshot &snapshot,
                          tsd_telemetry_sample_t *out) {
    if (!snapshot.temp_available && !snapshot.freq_available) {
        return false;
    }

    out->temp_available = snapshot.temp_available ? 1 : 0;
    out->freq_ratio_available = snapshot.freq_available ? 1 : 0;
    out->package_temp_millic = snapshot.temp_available
                                   ? static_cast<int32_t>(std::llround(snapshot.package_temp_c * 1000.0))
                                   : 0;
    out->freq_ratio_milli = snapshot.freq_available
                                ? static_cast<uint32_t>(std::llround(snapshot.freq_ratio))
                                : 0u;
    return true;
}

}  // namespace

extern "C" int tsd_telemetry_fusion_start(void) {
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    if (g_fusion && g_fusion->running()) {
        return 0;
    }

    g_manager = std::make_shared<telemetry::TelemetryBusManager>();
    telemetry::TelemetryFusionConfig config = default_config();
    g_fusion = std::make_unique<telemetry::TelemetryFusion>(config, g_manager);

    /*
     * The dispatcher workload is pinned to CPU 0. Seed the production fusion
     * bus from the same direct Linux telemetry helper so an empty collector
     * graph can never suppress otherwise available temperature/frequency data.
     * Future platform-specific collectors can still be registered on the same
     * manager and compete by timestamp/quality through TelemetryBus.
     */
    g_direct_helper_ready = tsd_telemetry_helper_init(&g_direct_helper, 0) == 0;

    g_fusion->start();
    return 0;
}

extern "C" void tsd_telemetry_fusion_stop(void) {
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    if (g_fusion) {
        g_fusion->stop();
        g_fusion.reset();
    }
    g_manager.reset();
    if (g_direct_helper_ready) {
        tsd_telemetry_helper_destroy(&g_direct_helper);
        g_direct_helper_ready = false;
    }
}

extern "C" int tsd_telemetry_fusion_sample(tsd_telemetry_sample_t *out) {
    if (!out) {
        return -1;
    }

    *out = tsd_telemetry_sample_t{};
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    if (!g_fusion) {
        return -1;
    }

    auto snapshot = g_fusion->latest_snapshot();
    if (snapshot && copy_usable_snapshot(*snapshot, out)) {
        return 0;
    }

    /*
     * Fail open to the authoritative direct helper rather than returning an
     * empty fused snapshot. Publish that sample into the bus so subsequent
     * fusion generations also carry the real hardware signals.
     */
    if (g_direct_helper_ready) {
        tsd_telemetry_sample_t direct{};
        if (tsd_telemetry_helper_sample(&g_direct_helper, &direct) == 0 &&
            (direct.temp_available || direct.freq_ratio_available)) {
            publish_direct_sample(direct);
            *out = direct;
            return 0;
        }
    }

    return -1;
}
