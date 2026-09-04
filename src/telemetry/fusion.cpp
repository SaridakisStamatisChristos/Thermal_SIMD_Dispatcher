#include <telemetry/fusion.h>

#include <cmath>
#include <exception>
#include <stdexcept>

#include <observability/telemetry_state.h>

#include <telemetry/bus.h>
#include <thermal/simd/logging.h>

namespace telemetry {

namespace {

void publish_fusion_running_state(int running) {
    tsd_fusion_telemetry_t telemetry{};
    telemetry.running = running ? 1 : 0;
    tsd_observability_update_fusion(&telemetry);
}

}  // namespace

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
    if (running_.load(std::memory_order_acquire) || stop_callers_ != 0 ||
        stop_join_in_progress_ || thread_.joinable()) {
        return;
    }

    running_.store(true, std::memory_order_release);
    try {
        thread_ = std::thread([this] { run(); });
        worker_thread_id_ = thread_.get_id();
    } catch (...) {
        worker_thread_id_ = std::thread::id{};
        running_.store(false, std::memory_order_release);
        publish_fusion_running_state(0);
        throw;
    }

    /* Lifecycle observability is published under the same mutex as the thread
     * state, so an older stop cannot overwrite a newer successful start. */
    publish_fusion_running_state(1);
}

void TelemetryFusion::stop() {
    std::thread joiner;
    const std::thread::id caller = std::this_thread::get_id();

    std::unique_lock<std::mutex> lock(thread_mutex_);
    ++stop_callers_;
    running_.store(false, std::memory_order_release);
    wake_cv_.notify_all();

    const bool caller_is_worker = worker_thread_id_ != std::thread::id{} &&
                                  worker_thread_id_ == caller;

    if (stop_join_in_progress_) {
        if (caller_is_worker) {
            /* An external stop already owns the join and is waiting for this
             * worker to return. Waiting here would deadlock that join. */
            --stop_callers_;
            thread_cv_.notify_all();
            return;
        }

        thread_cv_.wait(lock, [&] { return !stop_join_in_progress_; });
        /* The join owner publishes the stopped state before clearing the flag.
         * stop_callers_ keeps start() closed until every overlapping stop has
         * observed that completed lifecycle transition. */
        --stop_callers_;
        thread_cv_.notify_all();
        return;
    }

    if (!thread_.joinable()) {
        publish_fusion_running_state(0);
        worker_thread_id_ = std::thread::id{};
        --stop_callers_;
        thread_cv_.notify_all();
        return;
    }

    if (caller_is_worker) {
        /* A collector/provider may request stop from the worker itself. It
         * cannot join itself; leave the completed thread joinable so the next
         * external stop/destructor can reap it safely. */
        publish_fusion_running_state(0);
        --stop_callers_;
        thread_cv_.notify_all();
        return;
    }

    stop_join_in_progress_ = true;
    joiner = std::move(thread_);
    lock.unlock();

    joiner.join();

    lock.lock();
    worker_thread_id_ = std::thread::id{};
    /* Publish stopped before reopening lifecycle admission. This closes the
     * old race where start() could publish running=true and then be overwritten
     * by the previous stop's delayed running=false publication. */
    publish_fusion_running_state(0);
    stop_join_in_progress_ = false;
    --stop_callers_;
    thread_cv_.notify_all();
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
