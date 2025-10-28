#include <telemetry/history_store.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace telemetry {

namespace {
constexpr const char *kTempSuffix = ".tmp";
}

HistoryStore::HistoryStore(std::string path) : path_(std::move(path)) { load(); }

HistoryStore::~HistoryStore() { flush(); }

BaselineRecord HistoryStore::get(int socket) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(socket);
    if (it != records_.end()) {
        return it->second;
    }
    return {};
}

void HistoryStore::update(int socket, double sample_value) {
    auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    BaselineRecord &record = records_[socket];
    if (record.sample_count == 0) {
        record.sample_count = 1;
        record.baseline = sample_value;
        record.variance_accumulator = 0.0;
    } else {
        record.sample_count += 1;
        double delta = sample_value - record.baseline;
        record.baseline += delta / static_cast<double>(record.sample_count);
        double delta2 = sample_value - record.baseline;
        record.variance_accumulator += delta * delta2;
    }
    record.last_update = now;
    record.valid = true;
    flush_unlocked();
}

void HistoryStore::flush() const {
    std::lock_guard<std::mutex> lock(mutex_);
    flush_unlocked();
}

void HistoryStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    std::ifstream input(path_);
    if (!input.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream stream(line);
        int socket = 0;
        double baseline = 0.0;
        double variance_accumulator = 0.0;
        std::uint64_t sample_count = 0;
        long long timestamp = 0;
        if (!(stream >> socket >> baseline >> variance_accumulator >> sample_count >> timestamp)) {
            continue;
        }
        BaselineRecord record;
        record.baseline = baseline;
        record.variance_accumulator = variance_accumulator;
        record.sample_count = sample_count;
        record.valid = sample_count > 0;
        record.last_update = std::chrono::system_clock::time_point(std::chrono::seconds(timestamp));
        records_.emplace(socket, record);
    }
}

void HistoryStore::flush_unlocked() const {
    std::filesystem::path path(path_);
    if (path.empty()) {
        return;
    }

    {
        std::error_code ec;
        auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
    }

    std::filesystem::path temp_path = path;
    temp_path += kTempSuffix;

    {
        std::ofstream output(temp_path, std::ios::trunc);
        if (!output.is_open()) {
            return;
        }
        output.setf(std::ios::fixed, std::ios::floatfield);
        output.precision(17);
        for (const auto &entry : records_) {
            const auto &record = entry.second;
            long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                         record.last_update.time_since_epoch())
                                         .count();
            output << entry.first << ' ' << record.baseline << ' ' << record.variance_accumulator << ' '
                   << record.sample_count << ' ' << timestamp << '\n';
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp_path, path_, ec);
    if (ec) {
        std::filesystem::remove(path_, ec);
        ec.clear();
        std::filesystem::rename(temp_path, path_, ec);
        if (ec) {
            std::filesystem::remove(temp_path, ec);
        }
    }
}

} // namespace telemetry

