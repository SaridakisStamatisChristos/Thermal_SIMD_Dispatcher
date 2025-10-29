#ifndef TSD_POLICY_MPC_CONTROLLER_H
#define TSD_POLICY_MPC_CONTROLLER_H

#include <chrono>
#include <deque>
#include <string>

#include <config/policy_config.h>
#include <thermal/simd/simd_width.h>
#include <thermal/simd/thermal_eval_types.h>

#include "arx_model.h"

namespace tsd {
namespace policy {

struct TelemetrySample {
    double ratio_milli;
    double trimmed_ratio_milli;
    double severity_milli;
    double temperature_millic;
    bool temp_valid;
    simd_width_t width;
    std::chrono::steady_clock::time_point timestamp;
};

class MPCController {
public:
    MPCController();
    explicit MPCController(const tsd_policy_config &config);

    void reset(const tsd_policy_config &config);
    void pushSample(const tsd_thermal_eval_t &sample, simd_width_t width);
    bool recommend(simd_width_t current_width,
                   simd_width_t max_width,
                   simd_width_t &out_width);
    size_t sampleCount() const { return history_.size(); }

#ifdef TSD_ENABLE_TESTS
    void debugSetCoefficientPath(const std::string &path);
    double debugLastPrediction() const { return last_prediction_millic_; }
    bool debugPredictionValid() const { return last_prediction_valid_; }
#endif

private:
    bool loadCoefficients(bool log_success);
    void maybeReloadCoefficients();
    double computeForecastRatio(size_t horizon) const;
    double computeForecastTemperature(size_t horizon, size_t &valid_count, bool &used_model) const;
    double scoreWidth(simd_width_t candidate,
                      simd_width_t current,
                      size_t horizon,
                      double forecast_ratio,
                      double ratio_trend,
                      double forecast_temp,
                      double ratio_error) const;
    size_t historyLimit() const;

    tsd_policy_config config_{};
    std::deque<TelemetrySample> history_;
    std::string coeff_path_;
    ARXModel arx_model_;
    std::chrono::milliseconds staleness_window_;
    double last_prediction_millic_;
    bool last_prediction_valid_;
    size_t history_limit_;
};

}  // namespace policy
}  // namespace tsd

#endif  // TSD_POLICY_MPC_CONTROLLER_H
