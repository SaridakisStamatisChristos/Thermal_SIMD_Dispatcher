#include <cassert>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <telemetry/evaluator.h>
#include <telemetry/history_store.h>
#include <telemetry/sensors.h>

using telemetry::AmdHsmpAdapter;
using telemetry::AcpiPmtAdapter;
using telemetry::BaselineRecord;
using telemetry::HfiPmtAdapter;
using telemetry::HistoryStore;
using telemetry::SensorEvaluator;
using telemetry::SensorSample;

int main() {
    namespace fs = std::filesystem;

    fs::path path = fs::temp_directory_path() / "tsd_telemetry_history_store.dat";
    fs::remove(path);

    auto history = std::make_shared<HistoryStore>(path.string());

    SensorSample stable_sample{};
    stable_sample.value = 70.0;
    stable_sample.health = 0.9;
    stable_sample.quality = 0.8;
    stable_sample.valid = true;

    auto stable_sensor = std::make_shared<HfiPmtAdapter>(
        [stable_sample](int) -> std::optional<SensorSample> { return stable_sample; });

    int volatile_calls = 0;
    auto volatile_sensor = std::make_shared<AmdHsmpAdapter>(
        [&volatile_calls](int) -> std::optional<SensorSample> {
            ++volatile_calls;
            if (volatile_calls > 2) {
                return std::nullopt;
            }
            SensorSample sample{};
            sample.value = 80.0;
            sample.health = 0.2;
            sample.quality = (volatile_calls == 1) ? 0.1 : 0.05;
            sample.valid = true;
            return sample;
        });

    auto failing_sensor = std::make_shared<AcpiPmtAdapter>(
        [](int) -> std::optional<SensorSample> { return std::nullopt; });

    SensorEvaluator evaluator({stable_sensor, volatile_sensor, failing_sensor}, history);
    double evaluated = evaluator.evaluate_socket(0);

    const double kTolerance = 1e-6;
    double expected_weighted =
        (stable_sample.value * stable_sample.health * stable_sample.quality + 80.0 * 0.2 * 0.1) /
        (stable_sample.health * stable_sample.quality + 0.2 * 0.1);
    assert(std::fabs(evaluated - expected_weighted) < kTolerance);

    BaselineRecord record = history->get(0);
    assert(record.valid);
    assert(record.sample_count == 1);
    assert(std::fabs(record.baseline - expected_weighted) < kTolerance);
    assert(std::fabs(record.variance()) < kTolerance);

    SensorSample strong_sample{};
    strong_sample.value = 90.0;
    strong_sample.health = 1.0;
    strong_sample.quality = 1.0;
    strong_sample.valid = true;

    auto strong_sensor = std::make_shared<HfiPmtAdapter>(
        [strong_sample](int) -> std::optional<SensorSample> { return strong_sample; });

    SensorEvaluator evaluator_second({strong_sensor}, history);
    double second_value = evaluator_second.evaluate_socket(0);
    assert(std::fabs(second_value - strong_sample.value) < kTolerance);

    BaselineRecord record_after = history->get(0);
    assert(record_after.valid);
    assert(record_after.sample_count == 2);
    double expected_mean = (expected_weighted + strong_sample.value) / 2.0;
    assert(std::fabs(record_after.baseline - expected_mean) < kTolerance);
    double expected_variance =
        (std::pow(expected_weighted - expected_mean, 2) + std::pow(strong_sample.value - expected_mean, 2));
    assert(std::fabs(record_after.variance() - expected_variance) < kTolerance);

    history.reset();

    HistoryStore reloaded(path.string());
    BaselineRecord persisted = reloaded.get(0);
    assert(persisted.valid);
    assert(persisted.sample_count == 2);
    assert(std::fabs(persisted.baseline - expected_mean) < kTolerance);
    assert(std::fabs(persisted.variance() - expected_variance) < kTolerance);

    auto degraded_sensor = std::make_shared<AcpiPmtAdapter>(
        [](int) -> std::optional<SensorSample> {
            SensorSample sample{};
            sample.value = 10.0;
            sample.health = 0.0;
            sample.quality = 0.0;
            sample.valid = false;
            return sample;
        });

    auto history_ptr = std::make_shared<HistoryStore>(path.string());
    SensorEvaluator evaluator_third({degraded_sensor}, history_ptr);
    double fallback_value = evaluator_third.evaluate_socket(0);
    assert(std::fabs(fallback_value - expected_mean) < kTolerance);

    history_ptr.reset();
    fs::remove(path);

    return 0;
}

