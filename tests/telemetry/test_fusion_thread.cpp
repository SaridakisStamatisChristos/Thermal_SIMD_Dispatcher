#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

#include <telemetry/fusion.h>
#include <thermal/simd/telemetry_fusion.h>
#include <thermal/simd/thermal_config.h>

namespace {

void test_cpp_fusion() {
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
}

void test_bridge_raw_safety_vs_filtered_control() {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    g_tsd_config.telemetry_interval_ms = 10;
    g_tsd_config.telemetry_max_skew_ms = 1000;
    g_tsd_config.telemetry_ewma_alpha = 0.25;

    tsd_telemetry_fusion_test_disable_direct_helper(1);
    assert(tsd_telemetry_fusion_start_for_cpu(0) == 0);

    tsd_telemetry_sample_t sample{};
    sample.temp_available = 1;
    sample.package_temp_millic = 60000;
    sample.freq_ratio_available = 1;
    sample.freq_ratio_milli = 1000;
    assert(tsd_telemetry_fusion_publish_sample(&sample) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    tsd_telemetry_sample_t first{};
    assert(tsd_telemetry_fusion_sample(&first) == 0);
    assert(first.temp_available == 1);
    assert(first.package_temp_millic == 60000);
    assert(first.filtered_temp_available == 1);
    assert(first.filtered_package_temp_millic == 60000);

    /* A thermal spike is immediate on the safety channel, smoothed only for control. */
    sample.package_temp_millic = 100000;
    assert(tsd_telemetry_fusion_publish_sample(&sample) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    tsd_telemetry_sample_t spike{};
    assert(tsd_telemetry_fusion_sample(&spike) == 0);
    assert(spike.temp_available == 1);
    assert(spike.package_temp_millic == 100000);
    assert(spike.filtered_temp_available == 1);
    assert(spike.filtered_package_temp_millic > 60000);
    assert(spike.filtered_package_temp_millic < 100000);
    tsd_telemetry_fusion_stop();

    /* alpha=0 is an explicit bypass, never a frozen first sample. */
    g_tsd_config.telemetry_ewma_alpha = 0.0;
    assert(tsd_telemetry_fusion_start_for_cpu(0) == 0);
    sample.package_temp_millic = 60000;
    assert(tsd_telemetry_fusion_publish_sample(&sample) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sample.package_temp_millic = 100000;
    assert(tsd_telemetry_fusion_publish_sample(&sample) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    tsd_telemetry_sample_t bypass{};
    assert(tsd_telemetry_fusion_sample(&bypass) == 0);
    assert(bypass.package_temp_millic == 100000);
    assert(bypass.filtered_package_temp_millic == 100000);
    tsd_telemetry_fusion_stop();
    tsd_telemetry_fusion_test_disable_direct_helper(0);
}

}  // namespace

int main() {
    test_cpp_fusion();
    test_bridge_raw_safety_vs_filtered_control();
    return 0;
}
