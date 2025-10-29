#include <telemetry/bus.h>

#include <algorithm>

#include <telemetry/collector.h>

namespace telemetry {

TelemetryBus::TelemetryBus() = default;

void TelemetryBus::publish(TelemetrySignal signal, const TelemetryReading &reading) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &slot = readings_[signal];
    if (!slot.valid || reading.quality >= slot.quality || reading.timestamp > slot.timestamp) {
        slot = reading;
    }
}

std::optional<TelemetryReading> TelemetryBus::latest(TelemetrySignal signal) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = readings_.find(signal);
    if (it == readings_.end()) {
        return std::nullopt;
    }
    return it->second;
}

TelemetryBusManager::TelemetryBusManager() = default;

void TelemetryBusManager::set_bus(std::shared_ptr<TelemetryBus> bus) { bus_ = std::move(bus); }

std::shared_ptr<TelemetryBus> TelemetryBusManager::bus() const { return bus_; }

void TelemetryBusManager::add_collector(std::shared_ptr<TelemetryCollector> collector) {
    if (!collector) {
        return;
    }
    collectors_.push_back(std::move(collector));
}

std::vector<std::shared_ptr<TelemetryCollector>> TelemetryBusManager::collectors() const {
    return collectors_;
}

void TelemetryBusManager::poll(std::chrono::steady_clock::time_point now) {
    if (!bus_) {
        return;
    }
    for (auto &collector : collectors_) {
        if (!collector) {
            continue;
        }
        collector->collect(*bus_, now);
    }
}

}  // namespace telemetry

