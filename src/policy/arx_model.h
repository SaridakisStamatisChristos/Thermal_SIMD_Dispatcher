#ifndef TSD_POLICY_ARX_MODEL_H
#define TSD_POLICY_ARX_MODEL_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace tsd {
namespace policy {

struct TelemetrySample;

class ARXModel {
public:
    ARXModel();

    bool loadFromFile(const std::string &path, std::string *error_out);
    double predict(const std::deque<TelemetrySample> &history, bool *ok) const;
    void updateResidual(double residual);
    void resetResidual();

    bool valid() const { return coefficients_loaded_; }
    std::size_t requiredHistory() const { return required_history_; }
    std::uint64_t stalenessWindowMs() const { return staleness_window_ms_; }

    /* Explicit control-input effects used to evaluate candidate width changes.
     * Positive temperature cost means wider SIMD projects hotter; positive
     * performance benefit means wider SIMD projects a lower CPI ratio. */
    double widthTemperatureMillicPerStep() const { return width_temperature_millic_per_step_; }
    double widthPerformanceBenefitMilliPerStep() const { return width_performance_benefit_milli_per_step_; }

private:
    bool parseContent(const std::string &content, std::string *error_out);

    double bias_;
    std::vector<double> temperature_coeffs_;
    std::vector<double> ratio_coeffs_;
    std::vector<double> severity_coeffs_;
    std::vector<double> trimmed_ratio_coeffs_;
    double ma_coefficient_;
    bool ma_enabled_;
    double last_residual_;
    bool residual_valid_;
    bool coefficients_loaded_;
    std::size_t required_history_;
    std::uint64_t staleness_window_ms_;
    double width_temperature_millic_per_step_;
    double width_performance_benefit_milli_per_step_;
};

}  // namespace policy
}  // namespace tsd

#endif  // TSD_POLICY_ARX_MODEL_H
