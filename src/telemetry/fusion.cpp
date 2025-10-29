#include <telemetry/fusion.h>

#include <cmath>
#include <stdexcept>

#include <observability/telemetry_state.h>

#include <telemetry/bus.h>

namespace telemetry {

TelemetrySnapshotRingBuffer::TelemetrySnapshotRingBuffer(std::size_t capacity)
    : capacity_(capacity ? capacity : 1),
      buffer_(capacity_),
      head_(capacity_ - 1),
      last_generation_(0) {}

void TelemetrySnapshotRingBuffer::publish(const TelemetrySnapshot &snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = (head_ + 1) % capacity_;
    buffer_[head_] = snapshot;
    last_generation_ = snapshot.generation;
    cv_.notify_all();
}

std::optional<TelemetrySnapshot> TelemetrySnapshotRingBuffer::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_generation_ == 0) {
        return std::nullopt;
    }
    return buffer_[head_];
}

std::optional<TelemetrySnapshot> TelemetrySnapshotRingBuffer::wait_for(
    uint64_t generation, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    if (generation == 0 || generation > last_generation_) {
        cv_.wait_for(lock, timeout, [&] { return last_generation_ >= generation && last_generation_ != 0; });
    }
    if (last_generation_ == 0 || last_generation_ < generation) {
        return std::nullopt;
    }
    return buffer_[head_];
}

TelemetryFusion::TelemetryFusion(TelemetryFusionConfig config,
                                 std::shared_ptr<TelemetryBusManager> bus_manager)
    : config_(config),
      bus_manager_(std::move(bus_manager)),
      bus_(std::make_shared<TelemetryBus>()),
      ring_(config_.ring_size),
      running_(false),
      generation_(0) {
    if (!bus_manager_) {
        bus_manager_ = std::make_shared<TelemetryBusManager>();
    }
    bus_manager_->set_bus(bus_);
}

TelemetryFusion::~TelemetryFusion() { stop(); }

void TelemetryFusion::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    std::lock_guard<std::mutex> lock(thread_mutex_);
    thread_ = std::thread([this] { run(); });
    tsd_fusion_telemetry_t telemetry{};
    telemetry.running = 1;
    tsd_observability_update_fusion(&telemetry);
}

void TelemetryFusion::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    tsd_fusion_telemetry_t telemetry{};
    telemetry.running = 0;
    tsd_observability_update_fusion(&telemetry);
}

bool TelemetryFusion::running() const { return running_.load(); }

void TelemetryFusion::register_collector(TelemetryCollectorPtr collector) {
    if (!collector) {
        return;
    }
    bus_manager_->add_collector(std::move(collector));
}

std::optional<TelemetrySnapshot> TelemetryFusion::latest_snapshot() const { return ring_.latest(); }

std::optional<TelemetrySnapshot> TelemetryFusion::wait_for_snapshot(
    uint64_t generation, std::chrono::milliseconds timeout) const {
    return ring_.wait_for(generation, timeout);
}

void TelemetryFusion::run() {
    auto next_run = std::chrono::steady_clock::now();
    while (running_.load()) {
        auto now = std::chrono::steady_clock::now();
        bus_manager_->poll(now);
        TelemetrySnapshot snapshot = fuse(now);
        ring_.publish(snapshot);

        next_run += config_.poll_interval;
        auto sleep_duration = next_run - std::chrono::steady_clock::now();
        if (sleep_duration > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(sleep_duration);
        } else {
            next_run = std::chrono::steady_clock::now();
        }
    }
}

TelemetrySnapshot TelemetryFusion::fuse(std::chrono::steady_clock::time_point now) {
    TelemetrySnapshot snapshot;
    snapshot.generation = ++generation_;
    snapshot.capture_time = now;

    snapshot.degraded = false;

    assign_value(snapshot, TelemetrySignal::kPackageTempC, snapshot.package_temp_c, snapshot.temp_available, now);
    assign_value(snapshot, TelemetrySignal::kFrequencyRatio, snapshot.freq_ratio, snapshot.freq_available, now);
    assign_value(snapshot, TelemetrySignal::kThermalCpi, snapshot.thermal_cpi, snapshot.cpi_available, now);
    assign_value(snapshot, TelemetrySignal::kPowerBudgetWatts, snapshot.power_budget_w, snapshot.power_available, now);

    if (!snapshot.temp_available || !snapshot.freq_available || !snapshot.cpi_available) {
        snapshot.degraded = true;
    }
    tsd_fusion_telemetry_t telemetry{};
    telemetry.running = running_.load() ? 1 : 0;
    telemetry.degraded = snapshot.degraded ? 1 : 0;
    telemetry.temp_available = snapshot.temp_available ? 1 : 0;
    telemetry.package_temp_c = snapshot.package_temp_c;
    telemetry.freq_available = snapshot.freq_available ? 1 : 0;
    telemetry.freq_ratio = snapshot.freq_ratio;
    telemetry.cpi_available = snapshot.cpi_available ? 1 : 0;
    telemetry.thermal_cpi = snapshot.thermal_cpi;
    telemetry.power_available = snapshot.power_available ? 1 : 0;
    telemetry.power_budget_w = snapshot.power_budget_w;
    tsd_observability_update_fusion(&telemetry);
    return snapshot;
}

bool TelemetryFusion::assign_value(TelemetrySnapshot &snapshot,
                                   TelemetrySignal signal,
                                   double &out_value,
                                   bool &out_flag,
                                   std::chrono::steady_clock::time_point now) {
    auto reading = bus_->latest(signal);
    if (!reading || !reading->valid) {
        out_flag = false;
        return false;
    }
    auto age = now - reading->timestamp;
    if (age > config_.freshness_window) {
        out_flag = false;
        return false;
    }
    out_value = reading->value;
    out_flag = true;
    return true;
}

}  // namespace telemetry

