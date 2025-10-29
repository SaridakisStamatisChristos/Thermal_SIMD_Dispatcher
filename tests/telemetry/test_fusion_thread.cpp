#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>

#include <telemetry/fusion.h>

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

    std::atomic<double> freq_value{2400.0};
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
    assert(snapshot->freq_ratio >= 2000.0);
    assert(snapshot->thermal_cpi >= 1000.0);
    assert(snapshot->power_budget_w > 0.0);

    temp_valid.store(false);
    auto degraded = fusion.wait_for_snapshot(snapshot->generation + 1, std::chrono::milliseconds(200));
    assert(degraded.has_value());
    assert(degraded->generation > snapshot->generation);
    assert(!degraded->temp_available);
    assert(degraded->degraded);

    fusion.stop();

    return 0;
}

