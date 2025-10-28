#ifndef TSD_TOOLS_POLICY_TUNER_H
#define TSD_TOOLS_POLICY_TUNER_H

#include <string>
#include <vector>

#include <config/policy_config.h>
#include <thermal/simd/simd_width.h>

namespace tools {
namespace policy_tuner {

struct TelemetrySample {
    double cpi_ratio{0.0};
    double package_temp_c{0.0};
    double dwell_ms{0.0};
    simd_width_t width{SIMD_SSE41};
};

struct TuningResult {
    tsd_policy_config policy{};
    double score{0.0};
    std::size_t sample_count{0};
};

std::vector<TelemetrySample> LoadArchive(const std::string &path);
TuningResult TuneFromSamples(const std::vector<TelemetrySample> &samples);
bool WritePolicyBundle(const std::string &path, const TuningResult &result);

} // namespace policy_tuner
} // namespace tools

#endif /* TSD_TOOLS_POLICY_TUNER_H */
