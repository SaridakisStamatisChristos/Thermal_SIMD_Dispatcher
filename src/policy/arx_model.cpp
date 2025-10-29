#include "arx_model.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "mpc_controller.h"

namespace tsd {
namespace policy {

namespace {

bool extractNumberToken(const std::string &content, std::size_t &pos, std::string *token) {
    std::size_t start = pos;
    while (pos < content.size()) {
        char ch = content[pos];
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.' || ch == 'e' ||
            ch == 'E') {
            ++pos;
            continue;
        }
        break;
    }
    if (pos == start) {
        return false;
    }
    if (token) {
        *token = content.substr(start, pos - start);
    }
    return true;
}

bool parseDoubleField(const std::string &content,
                      const std::string &key,
                      bool required,
                      double *out,
                      bool *found,
                      std::string *error_out) {
    if (!out) {
        if (error_out) {
            *error_out = "missing output pointer";
        }
        return false;
    }
    std::string needle = "\"" + key + "\"";
    std::size_t pos = content.find(needle);
    if (pos == std::string::npos) {
        if (found) {
            *found = false;
        }
        if (required && error_out) {
            *error_out = "missing field: " + key;
        }
        return !required;
    }
    if (found) {
        *found = true;
    }
    pos = content.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        if (error_out) {
            *error_out = "malformed field: " + key;
        }
        return false;
    }
    ++pos;
    while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos]))) {
        ++pos;
    }
    std::string token;
    if (!extractNumberToken(content, pos, &token)) {
        if (error_out) {
            *error_out = "invalid numeric value for field: " + key;
        }
        return false;
    }
    char *endptr = nullptr;
    errno = 0;
    double value = std::strtod(token.c_str(), &endptr);
    if (errno != 0 || !endptr || *endptr != '\0') {
        if (error_out) {
            *error_out = "failed to parse double for field: " + key;
        }
        return false;
    }
    *out = value;
    return true;
}

bool parseUint64Field(const std::string &content,
                      const std::string &key,
                      bool required,
                      std::uint64_t *out,
                      bool *found,
                      std::string *error_out) {
    double value = 0.0;
    bool local_found = false;
    if (!parseDoubleField(content, key, required, &value, &local_found, error_out)) {
        return false;
    }
    if (!local_found) {
        if (found) {
            *found = false;
        }
        return !required;
    }
    if (found) {
        *found = true;
    }
    if (value < 0.0) {
        if (error_out) {
            *error_out = "negative value for field: " + key;
        }
        return false;
    }
    *out = static_cast<std::uint64_t>(value + 0.5);
    return true;
}

bool parseDoubleArrayField(const std::string &content, const std::string &key, std::vector<double> *out,
                           std::string *error_out) {
    if (!out) {
        if (error_out) {
            *error_out = "missing output pointer";
        }
        return false;
    }
    std::string needle = "\"" + key + "\"";
    std::size_t pos = content.find(needle);
    if (pos == std::string::npos) {
        out->clear();
        return true;
    }
    pos = content.find('[', pos + needle.size());
    if (pos == std::string::npos) {
        if (error_out) {
            *error_out = "malformed array for field: " + key;
        }
        return false;
    }
    ++pos;
    out->clear();
    while (pos < content.size()) {
        while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos]))) {
            ++pos;
        }
        if (pos >= content.size()) {
            break;
        }
        if (content[pos] == ']') {
            ++pos;
            return true;
        }
        std::size_t start = pos;
        while (pos < content.size() && content[pos] != ',' && content[pos] != ']') {
            ++pos;
        }
        std::string token = content.substr(start, pos - start);
        std::size_t trimmed_start = token.find_first_not_of(" \t\n\r");
        std::size_t trimmed_end = token.find_last_not_of(" \t\n\r");
        if (trimmed_start == std::string::npos) {
            if (error_out) {
                *error_out = "empty entry in array for field: " + key;
            }
            return false;
        }
        token = token.substr(trimmed_start, trimmed_end - trimmed_start + 1);
        char *endptr = nullptr;
        errno = 0;
        double value = std::strtod(token.c_str(), &endptr);
        if (errno != 0 || !endptr || *endptr != '\0') {
            if (error_out) {
                *error_out = "invalid numeric entry in array for field: " + key;
            }
            return false;
        }
        out->push_back(value);
        if (pos < content.size() && content[pos] == ',') {
            ++pos;
        }
    }
    if (error_out) {
        *error_out = "unterminated array for field: " + key;
    }
    return false;
}

}  // namespace

ARXModel::ARXModel()
    : bias_(0.0),
      ma_coefficient_(0.0),
      ma_enabled_(false),
      last_residual_(0.0),
      residual_valid_(false),
      coefficients_loaded_(false),
      required_history_(1),
      staleness_window_ms_(500) {}

bool ARXModel::loadFromFile(const std::string &path, std::string *error_out) {
    std::ifstream stream(path);
    if (!stream.is_open()) {
        if (error_out) {
            *error_out = "unable to open coefficient file: " + path;
        }
        coefficients_loaded_ = false;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (buffer.fail()) {
        if (error_out) {
            *error_out = "failed to read coefficient file: " + path;
        }
        coefficients_loaded_ = false;
        return false;
    }
    std::string content = buffer.str();
    if (!parseContent(content, error_out)) {
        coefficients_loaded_ = false;
        return false;
    }
    coefficients_loaded_ = true;
    residual_valid_ = false;
    required_history_ = std::max<std::size_t>(1, temperature_coeffs_.size());
    required_history_ = std::max(required_history_, ratio_coeffs_.size());
    required_history_ = std::max(required_history_, severity_coeffs_.size());
    required_history_ = std::max(required_history_, trimmed_ratio_coeffs_.size());
    return true;
}

bool ARXModel::parseContent(const std::string &content, std::string *error_out) {
    double bias = 0.0;
    bool bias_found = false;
    if (!parseDoubleField(content, "bias", true, &bias, &bias_found, error_out) || !bias_found) {
        return false;
    }

    std::vector<double> temp_coeffs;
    if (!parseDoubleArrayField(content, "ar_temperature", &temp_coeffs, error_out)) {
        return false;
    }
    if (temp_coeffs.empty()) {
        if (error_out) {
            *error_out = "ar_temperature array must contain at least one coefficient";
        }
        return false;
    }

    std::vector<double> ratio_coeffs;
    if (!parseDoubleArrayField(content, "ratio", &ratio_coeffs, error_out)) {
        return false;
    }

    std::vector<double> severity_coeffs;
    if (!parseDoubleArrayField(content, "severity", &severity_coeffs, error_out)) {
        return false;
    }

    std::vector<double> trimmed_ratio_coeffs;
    if (!parseDoubleArrayField(content, "trimmed_ratio", &trimmed_ratio_coeffs, error_out)) {
        return false;
    }

    double ma_coeff = 0.0;
    bool ma_present = false;
    if (!parseDoubleField(content, "ma", false, &ma_coeff, &ma_present, error_out)) {
        return false;
    }

    std::uint64_t staleness_window = 0;
    bool staleness_found = false;
    if (!parseUint64Field(content, "staleness_window_ms", false, &staleness_window, &staleness_found, error_out)) {
        return false;
    }
    if (!staleness_found) {
        staleness_window = 500;
    }
    if (staleness_window == 0) {
        staleness_window = 1;
    }

    bias_ = bias;
    temperature_coeffs_ = std::move(temp_coeffs);
    ratio_coeffs_ = std::move(ratio_coeffs);
    severity_coeffs_ = std::move(severity_coeffs);
    trimmed_ratio_coeffs_ = std::move(trimmed_ratio_coeffs);
    ma_coefficient_ = ma_coeff;
    ma_enabled_ = ma_present;
    staleness_window_ms_ = staleness_window;
    return true;
}

double ARXModel::predict(const std::deque<TelemetrySample> &history, bool *ok) const {
    if (!coefficients_loaded_) {
        if (ok) {
            *ok = false;
        }
        return 0.0;
    }
    if (history.empty()) {
        if (ok) {
            *ok = false;
        }
        return 0.0;
    }
    std::size_t size = history.size();
    double prediction = bias_;
    bool used_temperature = false;

    for (std::size_t i = 0; i < temperature_coeffs_.size() && i < size; ++i) {
        const TelemetrySample &sample = history[size - 1 - i];
        if (!sample.temp_valid) {
            continue;
        }
        prediction += temperature_coeffs_[i] * sample.temperature_millic;
        used_temperature = true;
    }

    for (std::size_t i = 0; i < ratio_coeffs_.size() && i < size; ++i) {
        const TelemetrySample &sample = history[size - 1 - i];
        double ratio = sample.trimmed_ratio_milli > 0.0 ? sample.trimmed_ratio_milli : sample.ratio_milli;
        prediction += ratio_coeffs_[i] * ratio;
    }

    for (std::size_t i = 0; i < severity_coeffs_.size() && i < size; ++i) {
        const TelemetrySample &sample = history[size - 1 - i];
        prediction += severity_coeffs_[i] * sample.severity_milli;
    }

    for (std::size_t i = 0; i < trimmed_ratio_coeffs_.size() && i < size; ++i) {
        const TelemetrySample &sample = history[size - 1 - i];
        prediction += trimmed_ratio_coeffs_[i] * sample.trimmed_ratio_milli;
    }

    if (!used_temperature) {
        if (ok) {
            *ok = false;
        }
        return 0.0;
    }

    if (ma_enabled_ && residual_valid_) {
        prediction += ma_coefficient_ * last_residual_;
    }

    if (ok) {
        *ok = true;
    }
    return prediction;
}

void ARXModel::updateResidual(double residual) {
    last_residual_ = residual;
    residual_valid_ = true;
}

void ARXModel::resetResidual() {
    last_residual_ = 0.0;
    residual_valid_ = false;
}

}  // namespace policy
}  // namespace tsd
