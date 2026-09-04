#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace telemetry {

enum class TelemetrySignal {
    kPackageTempC,
    kFrequencyRatio,
    kThermalCpi,
    kPowerBudgetWatts,
};

struct TelemetryReading {
    double value = 0.0;
    bool valid = false;
    int quality = 0;
    std::chrono::steady_clock::time_point timestamp{};
    /* Stable producer identity. Empty means the legacy/default source. */
    std::string source;
};

class TelemetryBus {
public:
    TelemetryBus();

    void publish(TelemetrySignal signal, const TelemetryReading &reading);
    std::optional<TelemetryReading> latest(TelemetrySignal signal) const;
    std::vector<TelemetryReading> readings(TelemetrySignal signal) const;

private:
    using SourceMap = std::unordered_map<std::string, TelemetryReading>;
    using ReadingMap = std::unordered_map<TelemetrySignal, SourceMap>;

    mutable std::mutex mutex_;
    ReadingMap readings_;
};

class TelemetryCollector;

class TelemetryBusManager {
public:
    TelemetryBusManager();

    void set_bus(std::shared_ptr<TelemetryBus> bus);
    std::shared_ptr<TelemetryBus> bus() const;

    void add_collector(std::shared_ptr<TelemetryCollector> collector);
    std::vector<std::shared_ptr<TelemetryCollector>> collectors() const;
    void poll(std::chrono::steady_clock::time_point now);

private:
    mutable std::mutex mutex_;
    std::shared_ptr<TelemetryBus> bus_;
    std::vector<std::shared_ptr<TelemetryCollector>> collectors_;
};

}  // namespace telemetry
