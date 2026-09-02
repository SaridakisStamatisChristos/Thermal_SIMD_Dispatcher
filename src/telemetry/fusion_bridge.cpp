#include <thermal/simd/telemetry_fusion.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>

#include <telemetry/bus.h>
#include <telemetry/collector.h>
#include <telemetry/fusion.h>
#include <thermal/simd/logging.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_trampoline.h>

namespace {

std::mutex g_fusion_mutex;
std::unique_ptr<telemetry::TelemetryFusion> g_fusion;
std::shared_ptr<telemetry::TelemetryBusManager> g_manager;
tsd_telemetry_helper_t g_direct_helper{};
bool g_direct_helper_ready = false;
unsigned int g_fusion_users = 0;
int g_fusion_cpu = -1;
std::optional<double> g_smoothed_temp_c;
std::optional<double> g_smoothed_freq_ratio;
std::atomic<bool> g_fusion_running{false};
std::atomic<bool> g_temperature_upgrade_allowed{true};

telemetry::TelemetryFusionConfig default_config() {
    telemetry::TelemetryFusionConfig config;
    int interval_ms = g_tsd_config.telemetry_interval_ms > 0 ? g_tsd_config.telemetry_interval_ms : 50;
    int freshness_ms = g_tsd_config.telemetry_max_skew_ms >= 0 ? g_tsd_config.telemetry_max_skew_ms : 150;
    config.poll_interval = std::chrono::milliseconds(interval_ms);
    config.freshness_window = std::chrono::milliseconds(freshness_ms);
    config.ring_size = 128;
    return config;
}

double smooth_value(double raw, std::optional<double> &state) {
    double alpha = g_tsd_config.telemetry_ewma_alpha;
    if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0) {
        alpha = 1.0;
    }
    if (!state.has_value()) {
        state = raw;
        return raw;
    }
    state = alpha * raw + (1.0 - alpha) * *state;
    return *state;
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
        double raw_c = static_cast<double>(sample.package_temp_millic) / 1000.0;
        reading.value = smooth_value(raw_c, g_smoothed_temp_c);
        reading.valid = true;
        reading.quality = 100;
        reading.timestamp = now;
        bus->publish(telemetry::TelemetrySignal::kPackageTempC, reading);
        published = true;
    }
    if (sample.freq_ratio_available) {
        telemetry::TelemetryReading reading;
        double raw_ratio = static_cast<double>(sample.freq_ratio_milli);
        reading.value = smooth_value(raw_ratio, g_smoothed_freq_ratio);
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

void update_temperature_gate_unlocked(bool temperature_available) {
    g_temperature_upgrade_allowed.store(temperature_available, std::memory_order_release);
    if (!temperature_available &&
        std::atomic_load_explicit(&g_tsd_trampoline_initialized, std::memory_order_acquire) != 0 &&
        std::atomic_load_explicit(&g_tsd_current_width, std::memory_order_acquire) != SIMD_SSE41) {
        (void)tsd_trampoline_patch(SIMD_SSE41);
    }
}

int start_unlocked(int cpu) {
    if (cpu < 0) {
        return -1;
    }

    /*
     * A profile-manifest parser has never existed in the production path.
     * Reject a configured profile explicitly rather than accepting a knob
     * that has no effect. The caller will retain its CPU-local direct helper.
     */
    if (g_tsd_config.telemetry_profile_path[0] != '\0') {
        tsd_log_error("telemetry",
                      "telemetry profile manifests are not implemented; refusing fusion startup for profile=%s",
                      g_tsd_config.telemetry_profile_path);
        return -1;
    }

    if (g_fusion && g_fusion->running()) {
        /*
         * The service is intentionally process-wide and owns one direct CPU
         * helper. A second caller targeting another CPU must not silently
         * receive measurements from the first caller's CPU.
         */
        if (cpu != g_fusion_cpu) {
            return -1;
        }
        ++g_fusion_users;
        return 0;
    }

    g_manager = std::make_shared<telemetry::TelemetryBusManager>();
    telemetry::TelemetryFusionConfig config = default_config();
    g_fusion = std::make_unique<telemetry::TelemetryFusion>(config, g_manager);

    g_smoothed_temp_c.reset();
    g_smoothed_freq_ratio.reset();
    g_direct_helper_ready = tsd_telemetry_helper_init(&g_direct_helper, cpu) == 0;
    g_fusion_cpu = cpu;
    g_fusion->start();
    g_fusion_users = 1;
    g_fusion_running.store(true, std::memory_order_release);

    /* Establish the safety gate immediately rather than waiting one poll. */
    tsd_telemetry_sample_t initial{};
    bool has_temp = false;
    if (g_direct_helper_ready && tsd_telemetry_helper_sample(&g_direct_helper, &initial) == 0) {
        if (initial.temp_available || initial.freq_ratio_available) {
            (void)publish_sample_unlocked(initial);
        }
        has_temp = initial.temp_available != 0;
    }
    update_temperature_gate_unlocked(has_temp);
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
    g_fusion_cpu = -1;
    g_smoothed_temp_c.reset();
    g_smoothed_freq_ratio.reset();
    g_fusion_running.store(false, std::memory_order_release);
    g_temperature_upgrade_allowed.store(true, std::memory_order_release);
}

extern "C" int tsd_telemetry_fusion_publish_sample(const tsd_telemetry_sample_t *sample) {
    if (!sample) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_fusion_mutex);
    if (!g_fusion || !g_manager) {
        return -1;
    }
    bool published = publish_sample_unlocked(*sample);
    if (sample->temp_available) {
        update_temperature_gate_unlocked(true);
    }
    return published ? 0 : -1;
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

    update_temperature_gate_unlocked(out->temp_available != 0);
    return (fused_usable || direct_usable) ? 0 : -1;
}

extern "C" int tsd_telemetry_temperature_upgrade_allowed(void) {
    if (!g_fusion_running.load(std::memory_order_acquire)) {
        return 1;
    }
    return g_temperature_upgrade_allowed.load(std::memory_order_acquire) ? 1 : 0;
}
