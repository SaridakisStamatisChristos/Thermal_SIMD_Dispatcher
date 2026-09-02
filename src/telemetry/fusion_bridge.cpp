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
unsigned int g_fusion_users = 0;
int g_direct_cpu = -1;

telemetry::TelemetryFusionConfig default_config() {
    telemetry::TelemetryFusionConfig config;
    config.poll_interval = std::chrono::milliseconds(50);
    config.freshness_window = std::chrono::milliseconds(150);
    config.ring_size = 128;
    return config;
}

bool publish_sample_unlocked(const tsd_telemetry_sample_t &sample) {
    if (!g_manager) {
        return false;
    }
    auto bus = g_manager->bus();
    if (!bus) {
        return false;
    }

    bool published = false;
    const auto now = std::chrono::steady_clock::now();
    if (sample.temp_available) {
        telemetry::TelemetryReading reading;
        reading.value = static_cast<double>(sample.package_temp_millic) / 1000.0;
        reading.valid = true;
        reading.quality = 100;
        reading.timestamp = now;
        bus->publish(telemetry::TelemetrySignal::kPackageTempC, reading);
        published = true;
    }
    if (sample.freq_ratio_available) {
        telemetry::TelemetryReading reading;
        /* The fusion bus stores frequency ratio in milli-units end-to-end. */
        reading.value = static_cast<double>(sample.freq_ratio_milli);
        reading.valid = true;
        reading.quality = 100;
        reading.timestamp = now;
        bus->publish(telemetry::TelemetrySignal::kFrequencyRatio, reading);
        published = true;
    }
    return published;
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

void fill_missing_from_direct(tsd_telemetry_sample_t *out,
                              const tsd_telemetry_sample_t &direct) {
    if (!out->temp_available && direct.temp_available) {
        out->temp_available = 1;
        out->package_temp_millic = direct.package_temp_millic;
    }
    if (!out->freq_ratio_available && direct.freq_ratio_available) {
        out->freq_ratio_available = 1;
        out->freq_ratio_milli = direct.freq_ratio_milli;
    }
}

int start_unlocked(int cpu) {
    if (cpu < 0) {
        return -1;
    }

    if (g_fusion && g_fusion->running()) {
        ++g_fusion_users;
        return 0;
    }

    g_manager = std::make_shared<telemetry::TelemetryBusManager>();
    telemetry::TelemetryFusionConfig config = default_config();
    g_fusion = std::make_unique<telemetry::TelemetryFusion>(config, g_manager);

    g_direct_cpu = cpu;
    g_direct_helper_ready = tsd_telemetry_helper_init(&g_direct_helper, cpu) == 0;
    g_fusion->start();
    g_fusion_users = 1;
    return 0;
}

}  // namespace

extern "C" int tsd_telemetry_fusion_start(void) {
    return tsd_telemetry_fusion_start_for_cpu(0);
}

extern "C" int tsd_telemetry_fusion_start_for_cpu(int cpu) {
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    return start_unlocked(cpu);
}

extern "C" void tsd_telemetry_fusion_stop(void) {
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    if (g_fusion_users > 1) {
        --g_fusion_users;
        return;
    }
    g_fusion_users = 0;
    if (g_fusion) {
        g_fusion->stop();
        g_fusion.reset();
    }
    g_manager.reset();
    if (g_direct_helper_ready) {
        tsd_telemetry_helper_destroy(&g_direct_helper);
        g_direct_helper_ready = false;
    }
    g_direct_cpu = -1;
}

extern "C" int tsd_telemetry_fusion_publish_sample(const tsd_telemetry_sample_t *sample) {
    if (!sample) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    if (!g_fusion || !g_manager) {
        return -1;
    }
    return publish_sample_unlocked(*sample) ? 0 : -1;
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

    /*
     * Always advance the direct helper's retry/recovery state. If we only
     * sampled it when the fused snapshot was completely empty, one healthy
     * signal could mask the recovery of another indefinitely.
     */
    tsd_telemetry_sample_t direct{};
    bool direct_usable = false;
    if (g_direct_helper_ready && tsd_telemetry_helper_sample(&g_direct_helper, &direct) == 0) {
        direct_usable = direct.temp_available || direct.freq_ratio_available;
        if (direct_usable) {
            (void)publish_sample_unlocked(direct);
        }
    }

    auto snapshot = g_fusion->latest_snapshot();
    bool fused_usable = snapshot && copy_usable_snapshot(*snapshot, out);
    if (direct_usable) {
        fill_missing_from_direct(out, direct);
    }

    return (fused_usable || direct_usable) ? 0 : -1;
}
