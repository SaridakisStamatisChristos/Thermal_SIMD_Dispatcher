#ifndef TSD_POLICY_MPC_CONTROLLER_H
#define TSD_POLICY_MPC_CONTROLLER_H

#include <deque>

#include <config/policy_config.h>
#include <thermal/simd/simd_width.h>
#include <thermal/simd/thermal_eval_types.h>

namespace tsd {
namespace policy {

struct TelemetrySample {
    double ratio_milli;
    double trimmed_ratio_milli;
    double severity_milli;
    double temperature_millic;
    bool temp_valid;
    simd_width_t width;
};

class MPCController {
public:
    MPCController();
    explicit MPCController(const tsd_policy_config &config);

    void reset(const tsd_policy_config &config);
    void pushSample(const tsd_thermal_eval_t &sample, simd_width_t width);
    bool recommend(simd_width_t current_width,
                   simd_width_t max_width,
                   simd_width_t &out_width) const;
    size_t sampleCount() const { return history_.size(); }

private:
    double computeForecastRatio(size_t horizon) const;
    double computeForecastTemperature(size_t horizon, size_t &valid_count) const;
    double scoreWidth(simd_width_t candidate,
                      simd_width_t current,
                      size_t horizon,
                      double forecast_ratio,
                      double ratio_trend,
                      double forecast_temp,
                      double ratio_error) const;

    tsd_policy_config config_{};
    std::deque<TelemetrySample> history_;
};

}  // namespace policy
}  // namespace tsd

#endif  // TSD_POLICY_MPC_CONTROLLER_H
