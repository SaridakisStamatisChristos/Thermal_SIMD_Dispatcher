#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace telemetry {

struct BaselineRecord {
    double baseline = 0.0;
    double variance_accumulator = 0.0;
    std::uint64_t sample_count = 0;
    std::chrono::system_clock::time_point last_update{};
    bool valid = false;

    double variance() const {
        if (sample_count <= 1) {
            return 0.0;
        }
        return variance_accumulator / static_cast<double>(sample_count - 1);
    }
};

class HistoryStore {
public:
    explicit HistoryStore(std::string path);
    ~HistoryStore();

    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    BaselineRecord get(int socket) const;
    void update(int socket, double sample_value);
    void flush() const;

private:
    void load();
    void flush_unlocked() const;

    std::string path_;
    mutable std::mutex mutex_;
    std::unordered_map<int, BaselineRecord> records_;
};

} // namespace telemetry

