#include <tools/policy_tuner/tuner.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tools {
namespace policy_tuner {

namespace {

simd_width_t ParseWidth(std::string_view token) {
    if (token == "avx512") {
        return SIMD_AVX512;
    }
    if (token == "avx2") {
        return SIMD_AVX2;
    }
    return SIMD_SSE41;
}

bool SplitCSVLine(const std::string &line, std::vector<std::string> &out) {
    out.clear();
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty() && item.front() == ' ') {
            item.erase(item.begin());
        }
        if (!item.empty() && item.back() == ' ') {
            item.pop_back();
        }
        out.push_back(item);
    }
    return !out.empty();
}

double ClampDouble(double value, double min_value, double max_value) {
    return std::max(min_value, std::min(max_value, value));
}

} // namespace

std::vector<TelemetrySample> LoadArchive(const std::string &path) {
    std::vector<TelemetrySample> samples;
    std::ifstream input(path);
    if (!input.is_open()) {
        return samples;
    }

    std::string line;
    std::vector<std::string> tokens;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (!SplitCSVLine(line, tokens)) {
            continue;
        }
        if (tokens.size() < 5) {
            continue;
        }
        try {
            TelemetrySample sample;
            sample.cpi_ratio = std::stod(tokens[3]);
            sample.package_temp_c = std::stod(tokens[4]);
            sample.dwell_ms = (tokens.size() > 5) ? std::stod(tokens[5]) : 0.0;
            sample.width = ParseWidth(tokens[2]);
            samples.push_back(sample);
        } catch (const std::exception &) {
            continue;
        }
    }

    return samples;
}

TuningResult TuneFromSamples(const std::vector<TelemetrySample> &samples) {
    TuningResult result{};
    tsd_policy_config_set_defaults(&result.policy);
    result.sample_count = samples.size();
    if (samples.empty()) {
        tsd_policy_config_apply_bounds(&result.policy);
        return result;
    }

    double ratio_sum = 0.0;
    double dwell_sum = 0.0;
    double max_temp = -1e9;
    double min_ratio = std::numeric_limits<double>::infinity();
    double max_ratio = 0.0;

    for (const auto &sample : samples) {
        ratio_sum += sample.cpi_ratio;
        dwell_sum += sample.dwell_ms;
        max_temp = std::max(max_temp, sample.package_temp_c);
        min_ratio = std::min(min_ratio, sample.cpi_ratio);
        max_ratio = std::max(max_ratio, sample.cpi_ratio);
    }

    double ratio_mean = ratio_sum / static_cast<double>(samples.size());
    double dwell_mean = dwell_sum / static_cast<double>(samples.size());

    double slo_ratio = ClampDouble(ratio_mean * 1000.0, 900.0, 4000.0);
    result.policy.slo_ratio_milli = static_cast<uint32_t>(std::lround(slo_ratio));

    double slo_temp = ClampDouble(max_temp * 1000.0, 60000.0, 100000.0);
    result.policy.slo_temp_millic = static_cast<int32_t>(std::lround(slo_temp));

    double baseline_penalty = ClampDouble(dwell_mean / 12.0 + 400.0, 200.0, 4000.0);
    result.policy.transition_penalty_up_milli = static_cast<uint32_t>(std::lround(baseline_penalty));
    result.policy.transition_penalty_down_milli = static_cast<uint32_t>(
        std::lround(ClampDouble(baseline_penalty * 1.25, 250.0, 6000.0)));

    double spread = std::max(1.0, max_ratio - min_ratio);
    uint32_t horizon = static_cast<uint32_t>(
        std::clamp<std::uint32_t>(static_cast<uint32_t>(std::lround(spread * 3.0)), 1u, 60u));
    result.policy.forecast_horizon = horizon;

    tsd_policy_config_apply_bounds(&result.policy);

    result.score = ratio_mean * (1.0 + (max_temp / 100.0)) - dwell_mean / 1000.0;
    return result;
}

bool WritePolicyBundle(const std::string &path, const TuningResult &result) {
    std::filesystem::path target(path);
    auto parent = target.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    output << std::fixed << std::setprecision(3);
    output << "{\n";
    output << "  \"metadata\": {\n";
    output << "    \"score\": " << result.score << ",\n";
    output << "    \"samples\": " << result.sample_count << "\n";
    output << "  },\n";
    output << "  \"policy\": {\n";
    output << "    \"slo_ratio_milli\": " << result.policy.slo_ratio_milli << ",\n";
    output << "    \"slo_temp_millic\": " << result.policy.slo_temp_millic << ",\n";
    output << "    \"transition_penalty_up_milli\": " << result.policy.transition_penalty_up_milli << ",\n";
    output << "    \"transition_penalty_down_milli\": " << result.policy.transition_penalty_down_milli << ",\n";
    output << "    \"forecast_horizon\": " << result.policy.forecast_horizon << "\n";
    output << "  }\n";
    output << "}\n";
    return true;
}

} // namespace policy_tuner
} // namespace tools

