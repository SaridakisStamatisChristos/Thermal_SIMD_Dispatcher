#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <telemetry/bus.h>
#include <telemetry/collector.h>

namespace telemetry {

struct TelemetrySnapshot {
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point capture_time{};
    bool degraded = false;

    bool temp_available = false;
    double package_temp_c = 0.0;

    bool freq_available = false;
    double freq_ratio = 0.0;

    bool cpi_available = false;
    double thermal_cpi = 0.0;

    bool power_available = false;
    double power_budget_w = 0.0;
};

struct TelemetryFusionConfig {
    std::chrono::milliseconds poll_interval{50};
    std::chrono::milliseconds freshness_window{100};
    std::size_t ring_size = 64;
};

class TelemetrySnapshotRingBuffer {
public:
    explicit TelemetrySnapshotRingBuffer(std::size_t capacity);

    void publish(const TelemetrySnapshot &snapshot);
    std::optional<TelemetrySnapshot> latest() const;
    std::optional<TelemetrySnapshot> wait_for(uint64_t generation,
                                              std::chrono::milliseconds timeout) const;

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<TelemetrySnapshot> buffer_;
    uint64_t head_;
    uint64_t last_generation_;
};

class TelemetryFusion {
public:
    TelemetryFusion(TelemetryFusionConfig config,
                    std::shared_ptr<TelemetryBusManager> bus_manager);
    ~TelemetryFusion();

    TelemetryFusion(const TelemetryFusion &) = delete;
    TelemetryFusion &operator=(const TelemetryFusion &) = delete;

    void start();
    void stop();
    bool running() const;

    void register_collector(TelemetryCollectorPtr collector);

    std::optional<TelemetrySnapshot> latest_snapshot() const;
    std::optional<TelemetrySnapshot> wait_for_snapshot(uint64_t generation,
                                                       std::chrono::milliseconds timeout) const;

private:
    void run();
    TelemetrySnapshot fuse(std::chrono::steady_clock::time_point now);
    bool assign_value(TelemetrySignal signal,
                      double &out_value,
                      bool &out_flag,
                      std::chrono::steady_clock::time_point now);

    TelemetryFusionConfig config_;
    std::shared_ptr<TelemetryBusManager> bus_manager_;
    std::shared_ptr<TelemetryBus> bus_;
    TelemetrySnapshotRingBuffer ring_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> generation_;
    mutable std::mutex thread_mutex_;
    std::thread thread_;
};

}  // namespace telemetry
