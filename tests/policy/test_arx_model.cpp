#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <thread>
#include <deque>
#include <cstdint>

#include <thermal/simd/metrics.h>
#include <thermal/simd/simd_width.h>

#include "policy/arx_model.h"
#include "policy/mpc_controller.h"

using tsd::policy::ARXModel;
using tsd::policy::MPCController;
using tsd::policy::TelemetrySample;

namespace {

std::filesystem::path writeCoefficients(const std::string &name, const std::string &content) {
    auto dir = std::filesystem::temp_directory_path() / "tsd_policy_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::ofstream stream(path);
    stream << content;
    stream.close();
    return path;
}

void populateSample(tsd_thermal_eval_t &sample, uint32_t ratio, int32_t temp_millic) {
    sample = tsd_thermal_eval_t{};
    sample.ratio_milli = ratio;
    sample.trimmed_ratio_milli = ratio;
    sample.severity_milli = ratio;
    sample.thermal_severity_milli = ratio;
    sample.package_temp_millic = temp_millic;
    sample.temp_available = 1;
}

void test_arx_prediction_basic() {
    auto path = writeCoefficients(
        "coeff_basic.json",
        R"JSON({
  "bias": 100.0,
  "ar_temperature": [0.5, 0.1],
  "ratio": [0.01],
  "severity": [0.02],
  "ma": 0.0,
  "staleness_window_ms": 250
})JSON");

    ARXModel model;
    std::string error;
    bool ok = model.loadFromFile(path.string(), &error);
    assert(ok);
    (void)error;

    std::deque<TelemetrySample> history;
    TelemetrySample first{};
    first.temperature_millic = 80250.0;
    first.temp_valid = true;
    first.ratio_milli = 1450.0;
    first.trimmed_ratio_milli = 0.0;
    first.severity_milli = 1250.0;
    first.width = SIMD_AVX2;
    first.timestamp = std::chrono::steady_clock::now();
    history.push_back(first);

    TelemetrySample second = first;
    second.temperature_millic = 80600.0;
    second.ratio_milli = 1500.0;
    second.trimmed_ratio_milli = 0.0;
    second.severity_milli = 1300.0;
    second.timestamp = std::chrono::steady_clock::now();
    history.push_back(second);

    bool prediction_ok = false;
    double prediction = model.predict(history, &prediction_ok);
    assert(prediction_ok);

    double expected = 100.0 + 0.5 * second.temperature_millic + 0.1 * first.temperature_millic +
                      0.01 * second.ratio_milli + 0.02 * second.severity_milli;
    assert(std::fabs(prediction - expected) < 1e-3);
    assert(model.stalenessWindowMs() == 250);
}

void test_arx_requires_complete_history_and_temperature_lags() {
    auto path = writeCoefficients(
        "coeff_warmup.json",
        R"JSON({
  "bias": 50.0,
  "ar_temperature": [0.7, 0.2, 0.1],
  "ratio": [0.01, 0.005],
  "severity": [0.0],
  "ma": 0.0,
  "staleness_window_ms": 250
})JSON");

    ARXModel model;
    std::string error;
    assert(model.loadFromFile(path.string(), &error));
    assert(model.requiredHistory() == 3);

    TelemetrySample sample{};
    sample.temperature_millic = 80000.0;
    sample.temp_valid = true;
    sample.ratio_milli = 1400.0;
    sample.trimmed_ratio_milli = 1400.0;
    sample.timestamp = std::chrono::steady_clock::now();

    std::deque<TelemetrySample> history;
    history.push_back(sample);

    bool prediction_ok = true;
    assert(model.predict(history, &prediction_ok) == 0.0);
    assert(!prediction_ok);

    sample.temperature_millic = 80500.0;
    history.push_back(sample);
    prediction_ok = true;
    assert(model.predict(history, &prediction_ok) == 0.0);
    assert(!prediction_ok);

    sample.temperature_millic = 81000.0;
    history.push_back(sample);
    history[1].temp_valid = false;
    prediction_ok = true;
    assert(model.predict(history, &prediction_ok) == 0.0);
    assert(!prediction_ok);

    history[1].temp_valid = true;
    prediction_ok = false;
    double prediction = model.predict(history, &prediction_ok);
    assert(prediction_ok);
    assert(std::isfinite(prediction));
    assert(prediction > 0.0);
}

void test_arx_rejects_nonfinite_coefficients() {
    auto path = writeCoefficients(
        "coeff_nonfinite.json",
        R"JSON({
  "bias": 1e309,
  "ar_temperature": [1.0],
  "ratio": [0.0],
  "severity": [0.0],
  "ma": 0.0
})JSON");

    ARXModel model;
    std::string error;
    assert(!model.loadFromFile(path.string(), &error));
    assert(!error.empty());
}

void test_mpc_staleness_guard() {
    auto path = writeCoefficients(
        "coeff_stale.json",
        R"JSON({
  "bias": 0.0,
  "ar_temperature": [1.0],
  "ratio": [0.0],
  "severity": [0.0],
  "ma": 0.0,
  "staleness_window_ms": 1
})JSON");

    MPCController controller;
    tsd_policy_config cfg;
    tsd_policy_config_set_defaults(&cfg);
    cfg.forecast_horizon = 2;
    controller.reset(cfg);
    controller.debugSetCoefficientPath(path.string());

    tsd_metrics_snapshot_t before;
    tsd_metrics_snapshot(&before);

    tsd_thermal_eval_t sample{};
    populateSample(sample, 1400, 80000);
    controller.pushSample(sample, SIMD_AVX2);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    simd_width_t target = SIMD_AVX2;
    bool changed = controller.recommend(SIMD_AVX2, SIMD_AVX512, target);
    assert(!changed);
    assert(target == SIMD_AVX2);

    tsd_metrics_snapshot_t after;
    tsd_metrics_snapshot(&after);
    uint64_t stale_delta = after.counters[TSD_METRIC_PREDICTIVE_STALE_SAMPLES] -
                           before.counters[TSD_METRIC_PREDICTIVE_STALE_SAMPLES];
    assert(stale_delta == 1);
}

void test_mpc_explicit_reload() {
    auto base_path = writeCoefficients(
        "coeff_reload.json",
        R"JSON({
  "bias": 0.0,
  "ar_temperature": [0.5],
  "ratio": [0.0],
  "severity": [0.0],
  "ma": 0.0,
  "staleness_window_ms": 100
})JSON");

    MPCController controller;
    tsd_policy_config cfg;
    tsd_policy_config_set_defaults(&cfg);
    cfg.forecast_horizon = 3;
    controller.reset(cfg);
    controller.debugSetCoefficientPath(base_path.string());

    tsd_thermal_eval_t sample{};
    populateSample(sample, 1800, 82000);
    controller.pushSample(sample, SIMD_AVX2);
    populateSample(sample, 1700, 81500);
    controller.pushSample(sample, SIMD_AVX2);

    simd_width_t target = SIMD_AVX2;
    controller.recommend(SIMD_AVX2, SIMD_AVX512, target);
    double initial_prediction = controller.debugLastPrediction();

    std::ofstream stream(base_path);
    stream << R"JSON({
  "bias": 20000.0,
  "ar_temperature": [1.0],
  "ratio": [0.0],
  "severity": [0.0],
  "ma": 0.0,
  "staleness_window_ms": 100
})JSON";
    stream.close();

    assert(::setenv("TSD_PREDICTIVE_COEFF_PATH", base_path.c_str(), 1) == 0);
    assert(controller.reloadCoefficients());
    (void)::unsetenv("TSD_PREDICTIVE_COEFF_PATH");

    populateSample(sample, 1650, 81000);
    controller.pushSample(sample, SIMD_AVX2);
    target = SIMD_AVX2;
    controller.recommend(SIMD_AVX2, SIMD_AVX512, target);
    double reloaded_prediction = controller.debugLastPrediction();

    assert(reloaded_prediction > initial_prediction + 1000.0);
}

}  // namespace

int main() {
    test_arx_prediction_basic();
    test_arx_requires_complete_history_and_temperature_lags();
    test_arx_rejects_nonfinite_coefficients();
    test_mpc_staleness_guard();
    test_mpc_explicit_reload();
    std::printf("policy ARX model tests passed\n");
    return 0;
}
