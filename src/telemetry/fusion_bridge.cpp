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

bool g_raw_temp_valid = false;
int32_t g_raw_temp_millic = 0;
std::chrono::steady_clock::time_point g_raw_temp_at{};
bool g_raw_freq_valid = false;
uint32_t g_raw_freq_ratio_milli = 0;
std::chrono::steady_clock::time_point g_raw_freq_at{};

std::atomic<bool> g_fusion_running{false};
std::atomic<bool> g_temperature_upgrade_allowed{true};

bool runtime_config_initialized() {
    return g_tsd_config.check_interval_us > 0;
}

std::chrono::milliseconds freshness_window() {
    const bool initialized = runtime_config_initialized();
    int freshness_ms = initialized ? g_tsd_config.telemetry_max_skew_ms : 150;
    if (freshness_ms < 0) {
        freshness_ms = 150;
    }
    return std::chrono::milliseconds(freshness_ms);
}

telemetry::TelemetryFusionConfig default_config() {
    telemetry::TelemetryFusionConfig config;
    const bool initialized = runtime_config_initialized();
    int interval_ms = initialized && g_tsd_config.telemetry_interval_ms > 0
                          ? g_tsd_config.telemetry_interval_ms
                          : 50;
    config.poll_interval = std::chrono::milliseconds(interval_ms);
    config.freshness_window = freshness_window();
    config.ring_size = 128;
    return config;
}

double smoothing_alpha() {
    if (!runtime_config_initialized()) {
        return 1.0;
    }
    double alpha = g_tsd_config.telemetry_ewma_alpha;
    if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0) {
        return 1.0;
    }
    /* alpha == 0 means explicit filter bypass, never a frozen signal. */
    return alpha == 0.0 ? 1.0 : alpha;
}

double smooth_value(double raw, std::optional<double> &state) {
    double alpha = smoothing_alpha();
    if (!state.has_value()) {
        state = raw;
        return raw;
    }
    state = alpha * raw + (1.0 - alpha) * *state;
    return *state;
}

void record_raw_sample_unlocked(const tsd_telemetry_sample_t &sample,
                                std::chrono::steady_clock::time_point now) {
    if (sample.temp_available) {
        g_raw_temp_valid = true;
        g_raw_temp_millic = sample.package_temp_millic;
        g_raw_temp_at = now;
    }
    if (sample.freq_ratio_available) {
        g_raw_freq_valid = true;
        g_raw_freq_ratio_milli = sample.freq_ratio_milli;
        g_raw_freq_at = now;
    }
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
    record_raw_sample_unlocked(sample, now);

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

bool raw_is_fresh(std::chrono::steady_clock::time_point timestamp,
                  std::chrono::steady_clock::time_point now) {
    if (timestamp.time_since_epoch().count() == 0) {
        return false;
    }
    return now - timestamp <= freshness_window();
}

void copy_raw_cache_unlocked(tsd_telemetry_sample_t *out,
                             std::chrono::steady_clock::time_point now) {
    if (g_raw_temp_valid && raw_is_fresh(g_raw_temp_at, now)) {
        out->temp_available = 1;
        out->package_temp_millic = g_raw_temp_millic;
    }
    if (g_raw_freq_valid && raw_is_fresh(g_raw_freq_at, now)) {
        out->freq_ratio_available = 1;
        out->freq_ratio_milli = g_raw_freq_ratio_milli;
    }
}

bool copy_filtered_snapshot(const telemetry::TelemetrySnapshot &snapshot,
                            tsd_telemetry_sample_t *out) {
    if (!snapshot.temp_available && !snapshot.freq_available) {
        return false;
    }

    out->filtered_temp_available = snapshot.temp_available ? 1 : 0;
    out->filtered_freq_ratio_available = snapshot.freq_available ? 1 : 0;
    out->filtered_package_temp_millic = snapshot.temp_available
                                            ? static_cast<int32_t>(std::llround(snapshot.package_temp_c * 1000.0))
                                            : 0;
    out->filtered_freq_ratio_milli = snapshot.freq_available
                                         ? static_cast<uint32_t>(std::llround(snapshot.freq_ratio))
                                         : 0u;
    return true;
}

void fill_filtered_from_raw(tsd_telemetry_sample_t *out) {
    if (!out->filtered_temp_available && out->temp_available) {
        out->filtered_temp_available = 1;
        out->filtered_package_temp_millic = out->package_temp_millic;
    }
    if (!out->filtered_freq_ratio_available && out->freq_ratio_available) {
        out->filtered_freq_ratio_available = 1;
        out->filtered_freq_ratio_milli = out->freq_ratio_milli;
    }
}

void update_temperature_gate_unlocked(bool raw_temperature_available) {
    g_temperature_upgrade_allowed.store(raw_temperature_available, std::memory_order_release);
    if (!raw_temperature_available &&
        std::atomic_load_explicit(&g_tsd_trampoline_initialized, std::memory_order_acquire) != 0 &&
        std::atomic_load_explicit(&g_tsd_current_width, std::memory_order_acquire) != SIMD_SSE41) {
        (void)tsd_trampoline_patch(SIMD_SSE41);
    }
}

void reset_signal_state_unlocked() {
    g_smoothed_temp_c.reset();
    g_smoothed_freq_ratio.reset();
    g_raw_temp_valid = false;
    g_raw_temp_millic = 0;
    g_raw_temp_at = {};
    g_raw_freq_valid = false;
    g_raw_freq_ratio_milli = 0;
    g_raw_freq_at = {};
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
    if (runtime_config_initialized() && g_tsd_config.telemetry_profile_path[0] != '\0') {
        tsd_log_error("telemetry",
                      "telemetry profile manifests are not implemented; refusing fusion startup for profile=%s",
                      g_tsd_config.telemetry_profile_path);
        return -1;
    }

    if (g_fusion && g_fusion->running()) {
        /* The process-wide service owns exactly one workload CPU. */
        if (cpu != g_fusion_cpu) {
            return -1;
        }
        ++g_fusion_users;
        return 0;
    }

    g_manager = std::make_shared<telemetry::TelemetryBusManager>();
    telemetry::TelemetryFusionConfig config = default_config();
    g_fusion = std::make_unique<telemetry::TelemetryFusion>(config, g_manager);

    reset_signal_state_unlocked();
    g_direct_helper_ready = tsd_telemetry_helper_init(&g_direct_helper, cpu) == 0;
    g_fusion_cpu = cpu;
    g_fusion->start();
    g_fusion_users = 1;
    g_fusion_running.store(true, std::memory_order_release);

    /* Establish the raw safety gate immediately rather than waiting one poll. */
    tsd_telemetry_sample_t initial{};
    bool has_raw_temp = false;
    if (g_direct_helper_ready && tsd_telemetry_helper_sample(&g_direct_helper, &initial) == 0) {
        if (initial.temp_available || initial.freq_ratio_available) {
            (void)publish_sample_unlocked(initial);
        }
        has_raw_temp = initial.temp_available != 0;
    }
    update_temperature_gate_unlocked(has_raw_temp);
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
    reset_signal_state_unlocked();
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
    const auto now = std::chrono::steady_clock::now();
    tsd_telemetry_sample_t safety{};
    copy_raw_cache_unlocked(&safety, now);
    update_temperature_gate_unlocked(safety.temp_available != 0);
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
    if (g_direct_helper_ready && tsd_telemetry_helper_sample(&g_direct_helper, &direct) == 0 &&
        (direct.temp_available || direct.freq_ratio_available)) {
        /* Publishing records raw values before any smoothing is applied. */
        (void)publish_sample_unlocked(direct);
    }

    auto snapshot = g_fusion->latest_snapshot();
    bool filtered_usable = snapshot && copy_filtered_snapshot(*snapshot, out);

    const auto now = std::chrono::steady_clock::now();
    copy_raw_cache_unlocked(out, now);
    fill_filtered_from_raw(out);

    update_temperature_gate_unlocked(out->temp_available != 0);
    bool raw_usable = out->temp_available || out->freq_ratio_available;
    return (raw_usable || filtered_usable) ? 0 : -1;
}

extern "C" int tsd_telemetry_temperature_upgrade_allowed(void) {
    if (!g_fusion_running.load(std::memory_order_acquire)) {
        return 1;
    }
    return g_temperature_upgrade_allowed.load(std::memory_order_acquire) ? 1 : 0;
}
