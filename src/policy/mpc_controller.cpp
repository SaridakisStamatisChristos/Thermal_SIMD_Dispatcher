#include "mpc_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>

namespace tsd {
namespace policy {

namespace {
constexpr double kMinImprovement = 1.0;
constexpr double kRatioTrendWeight = 0.5;
constexpr double kTemperatureWeight = 0.001;
constexpr double kStabilityMargin = 0.25;

inline int widthIndex(simd_width_t width) {
    return static_cast<int>(width);
}

}  // namespace

MPCController::MPCController() {
    tsd_policy_config defaults;
    tsd_policy_config_set_defaults(&defaults);
    reset(defaults);
}

MPCController::MPCController(const tsd_policy_config &config) {
    reset(config);
}

void MPCController::reset(const tsd_policy_config &config) {
    config_ = config;
    history_.clear();
}

void MPCController::pushSample(const tsd_thermal_eval_t &sample, simd_width_t width) {
    TelemetrySample entry{};
    entry.ratio_milli = static_cast<double>(sample.ratio_milli);
    entry.trimmed_ratio_milli = static_cast<double>(sample.trimmed_ratio_milli);
    entry.severity_milli = static_cast<double>(sample.severity_milli);
    entry.temperature_millic = static_cast<double>(sample.package_temp_millic);
    entry.temp_valid = sample.temp_available != 0;
    entry.width = width;
    history_.push_back(entry);
    if (config_.forecast_horizon > 0) {
        while (history_.size() > static_cast<size_t>(config_.forecast_horizon)) {
            history_.pop_front();
        }
    }
}

double MPCController::computeForecastRatio(size_t horizon) const {
    if (history_.empty() || horizon == 0) {
        return 0.0;
    }
    double sum = 0.0;
    size_t count = std::min(history_.size(), horizon);
    auto begin = history_.end();
    std::advance(begin, -static_cast<long>(count));
    for (auto it = begin; it != history_.end(); ++it) {
        sum += it->trimmed_ratio_milli > 0.0 ? it->trimmed_ratio_milli : it->ratio_milli;
    }
    return sum / static_cast<double>(count);
}

double MPCController::computeForecastTemperature(size_t horizon, size_t &valid_count) const {
    if (history_.empty() || horizon == 0) {
        valid_count = 0;
        return 0.0;
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
    if (valid_count == 0) {
        return 0.0;
    }
    return sum / static_cast<double>(valid_count);
}

double MPCController::scoreWidth(simd_width_t candidate,
                                 simd_width_t current,
                                 size_t horizon,
                                 double forecast_ratio,
                                 double ratio_trend,
                                 double forecast_temp,
                                 double ratio_error) const {
    int step = widthIndex(current) - widthIndex(candidate);
    double ratio_projection = forecast_ratio + ratio_trend * kRatioTrendWeight;
    ratio_projection -= static_cast<double>(step) * ratio_error * 0.5;
    if (ratio_projection < 0.0) {
        ratio_projection = 0.0;
    }

    double ratio_cost = std::fabs(ratio_projection - static_cast<double>(config_.slo_ratio_milli));

    double temp_cost = 0.0;
    if (config_.slo_temp_millic != 0 && horizon > 0) {
        double temp_projection = forecast_temp;
        temp_projection -= static_cast<double>(step) * ratio_error * 0.1;
        temp_cost = std::fabs(temp_projection - static_cast<double>(config_.slo_temp_millic)) * kTemperatureWeight;
    }

    double transition_penalty = 0.0;
    if (candidate != current) {
        double base_penalty = (widthIndex(candidate) < widthIndex(current))
                                  ? static_cast<double>(config_.transition_penalty_down_milli)
                                  : static_cast<double>(config_.transition_penalty_up_milli);
        transition_penalty = base_penalty * std::abs(step);
    }

    return ratio_cost + temp_cost + transition_penalty;
}

bool MPCController::recommend(simd_width_t current_width,
                              simd_width_t max_width,
                              simd_width_t &out_width) const {
    if (history_.empty() || config_.forecast_horizon == 0) {
        return false;
    }
    size_t horizon = std::min(history_.size(), static_cast<size_t>(config_.forecast_horizon));
    if (horizon == 0) {
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
    double ratio_trend = (horizon > 1) ? (last_ratio - first_ratio) / static_cast<double>(horizon - 1) : 0.0;
    double ratio_error = forecast_ratio - static_cast<double>(config_.slo_ratio_milli);

    size_t temp_valid_count = 0;
    double forecast_temp = computeForecastTemperature(horizon, temp_valid_count);

    double best_cost = std::numeric_limits<double>::infinity();
    simd_width_t best_width = current_width;

    auto evaluate_width = [&](simd_width_t candidate) {
        if (widthIndex(candidate) > widthIndex(max_width)) {
            return;
        }
        double cost = scoreWidth(candidate, current_width, horizon, forecast_ratio,
                                 ratio_trend, forecast_temp, ratio_error);
        if (cost + kStabilityMargin < best_cost) {
            best_cost = cost;
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

    double current_cost = scoreWidth(current_width, current_width, horizon, forecast_ratio,
                                     ratio_trend, forecast_temp, ratio_error);
    if (current_cost - best_cost < kMinImprovement) {
        out_width = current_width;
        return false;
    }

    out_width = best_width;
    return true;
}

}  // namespace policy
}  // namespace tsd
