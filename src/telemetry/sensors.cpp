#include <telemetry/sensors.h>

#include <algorithm>
#include <exception>
#include <mutex>

#include <thermal/simd/logging.h>
#include <observability/metrics_exporter.h>

namespace telemetry {

namespace {
constexpr const char *kLogComponent = "telemetry";
}

SensorAdapterBase::SensorAdapterBase(std::string name,
                                     SensorSampleProvider sample_provider,
                                     SensorAvailabilityProvider availability_provider)
    : sample_provider_(std::move(sample_provider)),
      availability_provider_(std::move(availability_provider)),
      name_(std::move(name)) {}

std::string SensorAdapterBase::name() const { return name_; }

bool SensorAdapterBase::is_available(int socket) const {
    if (!availability_provider_) {
        return true;
    }
    try {
        return availability_provider_(socket);
    } catch (...) {
        tsd_log_warn(kLogComponent, "event=sensor_availability_error sensor=%s socket=%d", name_.c_str(), socket);
        return false;
    }
}

SensorSample SensorAdapterBase::sample(int socket) {
    if (!sample_provider_) {
        SensorSample sample = degraded_sample(socket);
        tsd_metrics_exporter_record_sensor_health(name_.c_str(), socket, sample.health, sample.quality, sample.valid ? 1 : 0);
        return sample;
    }

    try {
        auto maybe_sample = sample_provider_(socket);
        if (maybe_sample) {
            SensorSample sample = *maybe_sample;
            sample.health = std::clamp(sample.health, 0.0, 1.0);
            sample.quality = std::clamp(sample.quality, 0.0, 1.0);
            sample.valid = true;
            sample.timestamp = std::chrono::system_clock::now();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_good_samples_[socket] = sample;
            }
            tsd_metrics_exporter_record_sensor_health(name_.c_str(), socket, sample.health, sample.quality, sample.valid ? 1 : 0);
            return sample;
        }
    } catch (const std::exception &ex) {
        tsd_log_warn(kLogComponent,
                     "event=sensor_sample_error sensor=%s socket=%d error=%s",
                     name_.c_str(),
                     socket,
                     ex.what());
    } catch (...) {
        tsd_log_warn(kLogComponent,
                     "event=sensor_sample_error sensor=%s socket=%d error=unknown",
                     name_.c_str(),
                     socket);
    }

    SensorSample sample = degraded_sample(socket);
    tsd_metrics_exporter_record_sensor_health(name_.c_str(), socket, sample.health, sample.quality, sample.valid ? 1 : 0);
    return sample;
}

SensorSample SensorAdapterBase::degraded_sample(int socket) const {
    SensorSample sample{};
    sample.timestamp = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = last_good_samples_.find(socket);
    if (it != last_good_samples_.end()) {
        sample = it->second;
        sample.health *= 0.5;
        sample.quality *= 0.5;
        sample.valid = false;
    }
    return sample;
}

HfiPmtAdapter::HfiPmtAdapter(SensorSampleProvider sample_provider,
                             SensorAvailabilityProvider availability_provider)
    : SensorAdapterBase("hfi_pmt", std::move(sample_provider), std::move(availability_provider)) {}

AmdHsmpAdapter::AmdHsmpAdapter(SensorSampleProvider sample_provider,
                               SensorAvailabilityProvider availability_provider)
    : SensorAdapterBase("amd_hsmp", std::move(sample_provider), std::move(availability_provider)) {}

AcpiPmtAdapter::AcpiPmtAdapter(SensorSampleProvider sample_provider,
                               SensorAvailabilityProvider availability_provider)
    : SensorAdapterBase("acpi_pmt", std::move(sample_provider), std::move(availability_provider)) {}

} // namespace telemetry

