#include <telemetry/fusion.h>

#include <cmath>
#include <exception>
#include <stdexcept>

#include <observability/telemetry_state.h>

#include <telemetry/bus.h>
#include <thermal/simd/logging.h>

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
    if (last_generation_ == 0) return std::nullopt;
    return buffer_[head_];
}

std::optional<TelemetrySnapshot> TelemetrySnapshotRingBuffer::wait_for(
    uint64_t generation, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    if (generation == 0 || generation > last_generation_) {
        cv_.wait_for(lock, timeout, [&] { return last_generation_ >= generation && last_generation_ != 0; });
    }
    if (last_generation_ == 0 || last_generation_ < generation) return std::nullopt;
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
    if (!bus_manager_) bus_manager_ = std::make_shared<TelemetryBusManager>();
    bus_manager_->set_bus(bus_);
}

TelemetryFusion::~TelemetryFusion() {
    try {
        stop();
    } catch (...) {
    }
}

void TelemetryFusion::start() {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    if (running_.load(std::memory_order_acquire) || stop_join_in_progress_ || thread_.joinable()) {
        return;
    }

    running_.store(true, std::memory_order_release);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        throw;
    }

    tsd_fusion_telemetry_t telemetry{};
    telemetry.running = 1;
    tsd_observability_update_fusion(&telemetry);
}

void TelemetryFusion::stop() {
    std::thread joiner;
    bool self_stop = false;
    {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        running_.store(false, std::memory_order_release);
        wake_cv_.notify_all();

        if (stop_join_in_progress_) return;
        if (thread_.joinable()) {
            if (thread_.get_id() == std::this_thread::get_id()) {
                /* A collector/provider may request stop from the worker itself.
                 * It cannot join itself; leave the completed thread joinable so
                 * the next external stop/destructor can reap it safely. */
                self_stop = true;
            } else {
                stop_join_in_progress_ = true;
                joiner = std::move(thread_);
            }
        }
    }

    if (joiner.joinable()) {
        joiner.join();
        std::lock_guard<std::mutex> lock(thread_mutex_);
        stop_join_in_progress_ = false;
    }

    tsd_fusion_telemetry_t telemetry{};
    telemetry.running = 0;
    tsd_observability_update_fusion(&telemetry);

    (void)self_stop;
}

bool TelemetryFusion::running() const { return running_.load(std::memory_order_acquire); }

void TelemetryFusion::register_collector(TelemetryCollectorPtr collector) {
    if (!collector) return;
    bus_manager_->add_collector(std::move(collector));
}

std::optional<TelemetrySnapshot> TelemetryFusion::latest_snapshot() const { return ring_.latest(); }

std::optional<TelemetrySnapshot> TelemetryFusion::wait_for_snapshot(
    uint64_t generation, std::chrono::milliseconds timeout) const {
    return ring_.wait_for(generation, timeout);
}

void TelemetryFusion::run() {
    auto next_run = std::chrono::steady_clock::now();
    while (running_.load(std::memory_order_acquire)) {
        try {
            auto now = std::chrono::steady_clock::now();
            bus_manager_->poll(now);
            TelemetrySnapshot snapshot = fuse(now);
            ring_.publish(snapshot);
        } catch (const std::exception &ex) {
            tsd_log_error("telemetry", "fusion iteration failed: %s", ex.what());
            tsd_fusion_telemetry_t telemetry{};
            telemetry.running = running_.load(std::memory_order_acquire) ? 1 : 0;
            telemetry.degraded = 1;
            tsd_observability_update_fusion(&telemetry);
        } catch (...) {
            tsd_log_error("telemetry", "fusion iteration failed: unknown C++ exception");
            tsd_fusion_telemetry_t telemetry{};
            telemetry.running = running_.load(std::memory_order_acquire) ? 1 : 0;
            telemetry.degraded = 1;
            tsd_observability_update_fusion(&telemetry);
        }

        next_run += config_.poll_interval;
        auto now = std::chrono::steady_clock::now();
        if (next_run <= now) {
            next_run = now;
            continue;
        }
        std::unique_lock<std::mutex> wake_lock(wake_mutex_);
        wake_cv_.wait_until(wake_lock, next_run, [&] {
            return !running_.load(std::memory_order_acquire);
        });
    }
}

TelemetrySnapshot TelemetryFusion::fuse(std::chrono::steady_clock::time_point now) {
    TelemetrySnapshot snapshot;
    snapshot.generation = ++generation_;
    snapshot.capture_time = now;
    snapshot.degraded = false;

    assign_value(TelemetrySignal::kPackageTempC, snapshot.package_temp_c, snapshot.temp_available, now);
    assign_value(TelemetrySignal::kFrequencyRatio, snapshot.freq_ratio, snapshot.freq_available, now);
    assign_value(TelemetrySignal::kThermalCpi, snapshot.thermal_cpi, snapshot.cpi_available, now);
    assign_value(TelemetrySignal::kPowerBudgetWatts, snapshot.power_budget_w, snapshot.power_available, now);

    if (!snapshot.temp_available || !snapshot.freq_available) snapshot.degraded = true;

    tsd_fusion_telemetry_t telemetry{};
    telemetry.running = running_.load(std::memory_order_acquire) ? 1 : 0;
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

bool TelemetryFusion::assign_value(TelemetrySignal signal,
                                   double &out_value,
                                   bool &out_flag,
                                   std::chrono::steady_clock::time_point now) {
    const auto candidates = bus_->readings(signal);
    const TelemetryReading *best = nullptr;
    for (const auto &reading : candidates) {
        if (!reading.valid || !std::isfinite(reading.value)) continue;
        if (reading.timestamp.time_since_epoch().count() == 0 || reading.timestamp > now) continue;
        if (now - reading.timestamp > config_.freshness_window) continue;
        if (!best || reading.quality > best->quality ||
            (reading.quality == best->quality && reading.timestamp > best->timestamp)) {
            best = &reading;
        }
    }
    if (!best) {
        out_flag = false;
        return false;
    }
    out_value = best->value;
    out_flag = true;
    return true;
}

}  // namespace telemetry
