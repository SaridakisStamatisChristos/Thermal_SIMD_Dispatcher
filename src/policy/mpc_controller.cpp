#include "mpc_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include <thermal/simd/logging.h>
#include <thermal/simd/metrics.h>
#include <thermal/simd/thermal_config.h>

namespace tsd {
namespace policy {

namespace {
constexpr double kMinImprovement = 1.0;
constexpr double kDefaultRatioTrendWeight = 0.25;
constexpr double kRatioViolationWeight = 4.0;
constexpr double kRatioEfficiencyWeight = 0.05;
constexpr double kTemperatureWeight = 0.25;
constexpr double kStabilityMargin = 0.25;
constexpr double kHorizonDiscount = 0.90;
constexpr double kHardConstraintCost = 1.0e9;
constexpr std::chrono::milliseconds kDefaultStalenessWindow{500};

inline int widthIndex(simd_width_t width) { return static_cast<int>(width); }

const tsd_runtime_config &runtimeConfig() {
    return *tsd_runtime_config_active_snapshot();
}

#ifdef TSD_INSTALLED_COEFF_PATH
bool fileReadable(const char *path) {
    if (!path || !*path) return false;
    std::ifstream stream(path);
    return stream.good();
}
#endif

double runtimeTrendWeight() {
    double alpha = runtimeConfig().predictive_alpha;
    if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0) return kDefaultRatioTrendWeight;
    return alpha;
}

std::string resolveCoefficientPath() {
    const char *env = std::getenv("TSD_PREDICTIVE_COEFF_PATH");
    if (env && *env) return std::string(env);
    const auto &runtime = runtimeConfig();
    if (runtime.predictive_coeff_path[0] != '\0') return std::string(runtime.predictive_coeff_path);
#ifdef TSD_INSTALLED_COEFF_PATH
    if (fileReadable(TSD_INSTALLED_COEFF_PATH)) return std::string(TSD_INSTALLED_COEFF_PATH);
#endif
#ifdef TSD_DEFAULT_COEFF_PATH
    return std::string(TSD_DEFAULT_COEFF_PATH);
#else
    return std::string("config/controller_coeffs.json");
#endif
}

}  // namespace

MPCController::MPCController() {
    coeff_path_ = resolveCoefficientPath();
    tsd_policy_config defaults;
    tsd_policy_config_set_defaults(&defaults);
    reset(defaults);
}

MPCController::MPCController(const tsd_policy_config &config) {
    coeff_path_ = resolveCoefficientPath();
    reset(config);
}

void MPCController::reset(const tsd_policy_config &config) {
    config_ = config;
    history_.clear();
    arx_model_.resetResidual();
    staleness_window_ = kDefaultStalenessWindow;
    last_prediction_millic_ = 0.0;
    last_prediction_valid_ = false;
    history_limit_ = std::max<std::size_t>(1, config_.forecast_horizon);
    coeff_path_ = resolveCoefficientPath();
    if (!loadCoefficients(true)) tsd_log_warn("policy", "using fallback averaging forecast due to coefficient load failure");
}

void MPCController::pushSample(const tsd_thermal_eval_t &sample, simd_width_t width) {
    const bool control_temp_valid = sample.filtered_temp_available != 0 || sample.temp_available != 0;
    const double control_temp = sample.filtered_temp_available
                                    ? static_cast<double>(sample.filtered_package_temp_millic)
                                    : static_cast<double>(sample.package_temp_millic);

    if (last_prediction_valid_ && control_temp_valid) {
        double residual = control_temp - last_prediction_millic_;
        arx_model_.updateResidual(residual);
        double abs_residual = std::fabs(residual);
        std::uint64_t scaled = static_cast<std::uint64_t>(abs_residual + 0.5);
        tsd_metrics_add(TSD_METRIC_PREDICTIVE_ABS_ERROR_MILLIC, scaled);
        tsd_log_debug("policy", "forecast residual=%.2f (actual=%.2f predicted=%.2f)", residual,
                      control_temp, last_prediction_millic_);
    }
    last_prediction_valid_ = false;

    TelemetrySample entry{};
    entry.ratio_milli = static_cast<double>(sample.ratio_milli);
    entry.trimmed_ratio_milli = static_cast<double>(sample.trimmed_ratio_milli);
    entry.severity_milli = static_cast<double>(sample.severity_milli);
    entry.temperature_millic = control_temp;
    entry.temp_valid = control_temp_valid;
    entry.width = width;
    entry.timestamp = std::chrono::steady_clock::now();
    history_.push_back(entry);
    size_t limit = historyLimit();
    while (limit > 0 && history_.size() > limit) history_.pop_front();
}

double MPCController::computeForecastRatio(size_t horizon) const {
    if (history_.empty() || horizon == 0) return 0.0;
    double sum = 0.0;
    size_t count = std::min(history_.size(), horizon);
    auto begin = history_.end();
    std::advance(begin, -static_cast<long>(count));
    for (auto it = begin; it != history_.end(); ++it) {
        sum += it->trimmed_ratio_milli > 0.0 ? it->trimmed_ratio_milli : it->ratio_milli;
    }
    return sum / static_cast<double>(count);
}

double MPCController::computeForecastTemperature(size_t horizon, size_t &valid_count, bool &used_model) const {
    if (history_.empty() || horizon == 0) {
        valid_count = 0;
        used_model = false;
        return 0.0;
    }
    used_model = false;
    if (arx_model_.valid()) {
        bool ok = false;
        double prediction = arx_model_.predict(history_, &ok);
        if (ok) {
            const TelemetrySample &latest = history_.back();
            if (latest.temp_valid) prediction = std::max(prediction, latest.temperature_millic);
            valid_count = 1;
            used_model = true;
            return prediction;
        }
    }
    double sum = 0.0;
    valid_count = 0;
    size_t count = std::min(history_.size(), horizon);
    auto begin = history_.end();
    std::advance(begin, -static_cast<long>(count));
    for (auto it = begin; it != history_.end(); ++it) {
        if (it->temp_valid) {
            sum += it->temperature_millic;
            ++valid_count;
        }
    }
    return valid_count == 0 ? 0.0 : sum / static_cast<double>(valid_count);
}

double MPCController::scoreWidth(simd_width_t candidate,
                                 simd_width_t current,
                                 size_t horizon,
                                 double forecast_ratio,
                                 double ratio_trend,
                                 double forecast_temp) const {
    const int control_step = widthIndex(candidate) - widthIndex(current);
    const double ratio_benefit = arx_model_.widthPerformanceBenefitMilliPerStep();
    const double temp_effect = arx_model_.widthTemperatureMillicPerStep();

    double cost = 0.0;
    double discount = 1.0;
    const size_t rollout = std::max<std::size_t>(1, horizon);
    const auto &runtime = runtimeConfig();
    const bool hard_limit_valid = runtime.predictive_temp_ceiling_c >= 20 &&
                                  runtime.predictive_temp_ceiling_c <= 125;
    const double hard_limit = hard_limit_valid
                                  ? static_cast<double>(runtime.predictive_temp_ceiling_c) * 1000.0
                                  : -std::numeric_limits<double>::infinity();

    for (size_t step = 1; step <= rollout; ++step) {
        /* First-order actuator response: 50%, 75%, 87.5%, ... of the fitted
         * steady width effect. This creates an actual finite-horizon rollout
         * rather than applying one static offset to a one-step forecast. */
        const double response = 1.0 - std::pow(0.5, static_cast<double>(step));
        double ratio_projection = forecast_ratio +
            ratio_trend * runtimeTrendWeight() * static_cast<double>(step);
        ratio_projection -= static_cast<double>(control_step) * ratio_benefit * response;
        if (ratio_projection < 0.0) ratio_projection = 0.0;

        /* ratio_milli is a degradation ceiling: lower is better. Never punish
         * a prediction simply for being farther below the SLO. */
        const double ratio_excess = std::max(
            0.0, ratio_projection - static_cast<double>(config_.slo_ratio_milli));
        const double ratio_cost = ratio_excess * kRatioViolationWeight +
                                  ratio_projection * kRatioEfficiencyWeight;

        double temp_cost = 0.0;
        if (std::isfinite(forecast_temp)) {
            const double temp_projection = forecast_temp +
                static_cast<double>(control_step) * temp_effect * response;
            if (config_.slo_temp_millic != 0) {
                const double excess = temp_projection - static_cast<double>(config_.slo_temp_millic);
                if (excess > 0.0) temp_cost += excess * kTemperatureWeight;
            }
            /* Invalid hard-limit configuration fails closed inside scoring: a
             * wide candidate receives the hard constraint cost rather than
             * silently losing the thermal ceiling. */
            if (!hard_limit_valid && candidate > SIMD_SSE41) {
                temp_cost += kHardConstraintCost;
            } else if (hard_limit_valid && temp_projection > hard_limit) {
                temp_cost += kHardConstraintCost + (temp_projection - hard_limit);
            }
        } else if (!hard_limit_valid && candidate > SIMD_SSE41) {
            temp_cost += kHardConstraintCost;
        }

        cost += discount * (ratio_cost + temp_cost);
        discount *= kHorizonDiscount;
    }

    if (candidate != current) {
        double base_penalty = widthIndex(candidate) < widthIndex(current)
                                  ? static_cast<double>(config_.transition_penalty_down_milli)
                                  : static_cast<double>(config_.transition_penalty_up_milli);
        cost += base_penalty * std::abs(control_step);
    }
    return cost;
}

bool MPCController::recommend(simd_width_t current_width,
                              simd_width_t max_width,
                              simd_width_t &out_width) {
    if (history_.empty() || config_.forecast_horizon == 0) return false;
    size_t horizon = std::min(history_.size(), static_cast<size_t>(config_.forecast_horizon));
    if (horizon == 0) return false;

    auto now = std::chrono::steady_clock::now();
    const TelemetrySample &latest = history_.back();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - latest.timestamp);
    if (elapsed > staleness_window_) {
        tsd_metrics_increment(TSD_METRIC_PREDICTIVE_STALE_SAMPLES);
        tsd_log_warn("policy", "telemetry stale (%lld ms > %lld ms)",
                     static_cast<long long>(elapsed.count()),
                     static_cast<long long>(staleness_window_.count()));
        return false;
    }

    double forecast_ratio = computeForecastRatio(horizon);
    double first_ratio = history_.front().ratio_milli;
    double last_ratio = history_.back().ratio_milli;
    if (horizon > 1) {
        auto begin = history_.end();
        std::advance(begin, -static_cast<long>(horizon));
        first_ratio = begin->ratio_milli;
        last_ratio = history_.back().ratio_milli;
    }
    double ratio_trend = horizon > 1
                             ? (last_ratio - first_ratio) / static_cast<double>(horizon - 1)
                             : 0.0;

    size_t temp_valid_count = 0;
    bool used_model = false;
    double forecast_temp = computeForecastTemperature(horizon, temp_valid_count, used_model);
    if (used_model) tsd_metrics_increment(TSD_METRIC_PREDICTIVE_FORECASTS);
    else if (temp_valid_count == 0) forecast_temp = std::numeric_limits<double>::quiet_NaN();
    last_prediction_millic_ = forecast_temp;
    last_prediction_valid_ = used_model;

    double best_cost = std::numeric_limits<double>::infinity();
    simd_width_t best_width = current_width;
    auto evaluate_width = [&](simd_width_t candidate) {
        if (widthIndex(candidate) > widthIndex(max_width)) return;
        double candidate_cost = scoreWidth(candidate, current_width, horizon,
                                           forecast_ratio, ratio_trend, forecast_temp);
        if (candidate_cost + kStabilityMargin < best_cost) {
            best_cost = candidate_cost;
            best_width = candidate;
        }
    };

    evaluate_width(current_width);
    evaluate_width(SIMD_SSE41);
    evaluate_width(SIMD_AVX2);
    evaluate_width(SIMD_AVX512);

    if (best_width == current_width) {
        out_width = current_width;
        return false;
    }

    double current_cost = scoreWidth(current_width, current_width, horizon,
                                     forecast_ratio, ratio_trend, forecast_temp);
    if (current_cost - best_cost < kMinImprovement) {
        out_width = current_width;
        return false;
    }

    tsd_log_info("policy", "decision current=%d target=%d horizon=%zu forecast_ratio=%.2f forecast_temp=%.2f",
                 widthIndex(current_width), widthIndex(best_width), horizon, forecast_ratio, forecast_temp);
    tsd_metrics_increment(TSD_METRIC_PREDICTIVE_DECISIONS);
    out_width = best_width;
    return true;
}

bool MPCController::loadCoefficients(bool log_success) {
    std::string error;
    if (!arx_model_.loadFromFile(coeff_path_, &error)) {
        tsd_log_error("policy", "failed to load coefficients from %s: %s", coeff_path_.c_str(), error.c_str());
        tsd_metrics_increment(TSD_METRIC_PREDICTIVE_RELOAD_ERRORS);
        staleness_window_ = kDefaultStalenessWindow;
        history_limit_ = std::max<std::size_t>(1, config_.forecast_horizon);
        return false;
    }
    long long window_ms = static_cast<long long>(arx_model_.stalenessWindowMs());
    if (window_ms <= 0) window_ms = kDefaultStalenessWindow.count();
    staleness_window_ = std::chrono::milliseconds(window_ms);
    history_limit_ = std::max<std::size_t>(std::max<std::size_t>(1, config_.forecast_horizon), arx_model_.requiredHistory());
    arx_model_.resetResidual();
    tsd_metrics_increment(TSD_METRIC_PREDICTIVE_RELOADS);
    if (log_success) {
        tsd_log_info("policy", "loaded coefficients from %s (staleness=%lld ms, history=%zu, width_temp=%.1f, width_perf=%.1f)",
                     coeff_path_.c_str(), static_cast<long long>(staleness_window_.count()), history_limit_,
                     arx_model_.widthTemperatureMillicPerStep(), arx_model_.widthPerformanceBenefitMilliPerStep());
    }
    return true;
}

bool MPCController::reloadCoefficients() {
    coeff_path_ = resolveCoefficientPath();
    return loadCoefficients(false);
}

size_t MPCController::historyLimit() const {
    if (history_limit_ == 0) return std::max<std::size_t>(1, config_.forecast_horizon);
    return history_limit_;
}

#ifdef TSD_ENABLE_TESTS
void MPCController::debugSetCoefficientPath(const std::string &path) {
    coeff_path_ = path;
    loadCoefficients(false);
}
#endif

}  // namespace policy
}  // namespace tsd
