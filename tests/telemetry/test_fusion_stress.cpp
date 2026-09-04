#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

#include <telemetry/fusion.h>

int main() {
    using namespace telemetry;

    TelemetryFusionConfig config;
    config.poll_interval = std::chrono::milliseconds(2);
    config.freshness_window = std::chrono::milliseconds(15);
    config.ring_size = 32;

    auto manager = std::make_shared<TelemetryBusManager>();
    TelemetryFusion fusion(config, manager);

    std::atomic<int> perf_calls{0};
    PerfSampleProvider perf_provider = [&]() {
        PerfSample sample{};
        int count = perf_calls.fetch_add(1) + 1;
        sample.thermal_cpi = 1000.0 + static_cast<double>(count % 50);
        sample.freq_hint = 2100.0 + static_cast<double>(count % 25);
        sample.valid = true;
        return sample;
    };

    std::atomic<int> temp_calls{0};
    TemperatureSampleProvider temp_provider = [&]() {
        TemperatureSample sample{};
        int count = temp_calls.fetch_add(1) + 1;
        sample.package_temp_c = 60.0 + static_cast<double>(count % 10);
        sample.valid = true;
        return sample;
    };

    fusion.register_collector(std::make_shared<PerfCollector>(config.poll_interval, perf_provider, 1));
    fusion.register_collector(std::make_shared<MsrCollector>(config.poll_interval, temp_provider, 1));

    fusion.start();

    uint64_t last_generation = 0;
    for (int i = 0; i < 100; ++i) {
        auto snapshot = fusion.wait_for_snapshot(last_generation + 1, std::chrono::milliseconds(100));
        assert(snapshot.has_value());
        assert(snapshot->generation > last_generation);
        last_generation = snapshot->generation;
        assert(snapshot->temp_available);
        assert(snapshot->freq_available);
        assert(snapshot->cpi_available);
        assert(!snapshot->degraded);
    }

    fusion.stop();

    /* Exercise the class-level lifecycle contract itself, not just the C bridge
       mutex. Racing start/stop must never leak a joinable worker or terminate
       during destruction. A start that overlaps stop is allowed to lose. */
    for (int i = 0; i < 64; ++i) {
        std::thread starter([&] { fusion.start(); });
        std::thread stopper([&] { fusion.stop(); });
        starter.join();
        stopper.join();
        fusion.stop();
    }

    /* The object remains reusable after the race storm. */
    fusion.start();
    auto final_snapshot = fusion.wait_for_snapshot(last_generation + 1, std::chrono::milliseconds(200));
    assert(final_snapshot.has_value());
    assert(final_snapshot->generation > last_generation);
    fusion.stop();

    return 0;
}

