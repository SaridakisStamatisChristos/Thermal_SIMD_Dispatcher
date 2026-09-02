#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

#include <telemetry/fusion.h>
#include <thermal/simd/telemetry_fusion.h>

int main() {
    using namespace telemetry;

    TelemetryFusionConfig config;
    config.poll_interval = std::chrono::milliseconds(5);
    config.freshness_window = std::chrono::milliseconds(40);
    config.ring_size = 16;

    auto manager = std::make_shared<TelemetryBusManager>();
    TelemetryFusion fusion(config, manager);

    std::atomic<bool> temp_valid{true};
    std::atomic<double> temp_value{75.25};
    TemperatureSampleProvider temp_provider = [&]() {
        TemperatureSample sample{};
        sample.package_temp_c = temp_value.load();
        sample.valid = temp_valid.load();
        return sample;
    };

    std::atomic<double> freq_value{875.0};
    PerfSampleProvider perf_provider = [&]() {
        PerfSample sample{};
        sample.thermal_cpi = 1200.0;
        sample.freq_hint = freq_value.load();
        sample.valid = true;
        return sample;
    };

    std::atomic<double> power_value{125.5};
    RaplSampleProvider rapl_provider = [&]() {
        RaplSample sample{};
        sample.power_budget_w = power_value.load();
        sample.valid = true;
        return sample;
    };

    fusion.register_collector(std::make_shared<PerfCollector>(config.poll_interval, perf_provider, 1));
    fusion.register_collector(std::make_shared<MsrCollector>(config.poll_interval, temp_provider, 1));
    fusion.register_collector(std::make_shared<RaplCollector>(config.poll_interval * 2, rapl_provider, 1));

    fusion.start();

    auto snapshot = fusion.wait_for_snapshot(1, std::chrono::milliseconds(200));
    assert(snapshot.has_value());
    assert(snapshot->temp_available);
    assert(snapshot->freq_available);
    assert(snapshot->cpi_available);
    assert(snapshot->power_available);
    assert(!snapshot->degraded);
    assert(snapshot->package_temp_c > 70.0);
    assert(snapshot->freq_ratio >= 800.0 && snapshot->freq_ratio <= 1200.0);
    assert(snapshot->thermal_cpi >= 1000.0);
    assert(snapshot->power_budget_w > 0.0);

    temp_valid.store(false);
    auto degraded = fusion.wait_for_snapshot(snapshot->generation + 1, std::chrono::milliseconds(200));
    assert(degraded.has_value());
    assert(degraded->generation > snapshot->generation);
    assert(!degraded->temp_available);
    assert(degraded->degraded);

    fusion.stop();

    /*
     * Exercise the actual C production bridge. An explicitly published direct
     * sample must survive the bridge/fusion boundary with its units intact.
     */
    assert(tsd_telemetry_fusion_start() == 0);
    tsd_telemetry_sample_t direct{};
    direct.temp_available = 1;
    direct.package_temp_millic = 81250;
    direct.freq_ratio_available = 1;
    direct.freq_ratio_milli = 875;
    assert(tsd_telemetry_fusion_publish_sample(&direct) == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    tsd_telemetry_sample_t bridged{};
    assert(tsd_telemetry_fusion_sample(&bridged) == 0);
    assert(bridged.temp_available == 1);
    assert(bridged.package_temp_millic == 81250);
    assert(bridged.freq_ratio_available == 1);
    assert(bridged.freq_ratio_milli == 875);
    tsd_telemetry_fusion_stop();

    return 0;
}
