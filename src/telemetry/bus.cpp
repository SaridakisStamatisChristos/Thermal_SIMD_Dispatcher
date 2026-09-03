#include <telemetry/bus.h>

#include <algorithm>

#include <telemetry/collector.h>

namespace telemetry {

TelemetryBus::TelemetryBus() = default;

void TelemetryBus::publish(TelemetrySignal signal, const TelemetryReading &reading) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &slot = readings_[signal];

    /* Freshness is authoritative. A newer invalid observation must invalidate
     * an older good sample; otherwise sensor loss is hidden until the stale
     * reading ages out. Quality is only a tie-breaker at equal timestamps. */
    if (slot.timestamp.time_since_epoch().count() == 0 ||
        reading.timestamp > slot.timestamp ||
        (reading.timestamp == slot.timestamp && reading.quality > slot.quality)) {
        slot = reading;
    }
}

std::optional<TelemetryReading> TelemetryBus::latest(TelemetrySignal signal) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = readings_.find(signal);
    if (it == readings_.end()) return std::nullopt;
    return it->second;
}

TelemetryBusManager::TelemetryBusManager() = default;

void TelemetryBusManager::set_bus(std::shared_ptr<TelemetryBus> bus) {
    std::lock_guard<std::mutex> lock(mutex_);
    bus_ = std::move(bus);
}

std::shared_ptr<TelemetryBus> TelemetryBusManager::bus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bus_;
}

void TelemetryBusManager::add_collector(std::shared_ptr<TelemetryCollector> collector) {
    if (!collector) return;
    std::lock_guard<std::mutex> lock(mutex_);
    collectors_.push_back(std::move(collector));
}

std::vector<std::shared_ptr<TelemetryCollector>> TelemetryBusManager::collectors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return collectors_;
}

void TelemetryBusManager::poll(std::chrono::steady_clock::time_point now) {
    std::shared_ptr<TelemetryBus> bus;
    std::vector<std::shared_ptr<TelemetryCollector>> collectors;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bus = bus_;
        collectors = collectors_;
    }
    if (!bus) return;
    for (auto &collector : collectors) {
        if (collector) collector->collect(*bus, now);
    }
}

}  // namespace telemetry
