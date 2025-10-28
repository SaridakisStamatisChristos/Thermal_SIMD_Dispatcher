#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace telemetry {

struct SensorSample {
    double value = 0.0;
    double health = 0.0;
    double quality = 0.0;
    bool valid = false;
    std::chrono::system_clock::time_point timestamp{};
};

using SensorSampleProvider = std::function<std::optional<SensorSample>(int socket)>;
using SensorAvailabilityProvider = std::function<bool(int socket)>;

class SensorAdapter {
public:
    virtual ~SensorAdapter() = default;

    virtual std::string name() const = 0;
    virtual bool is_available(int socket) const = 0;
    virtual SensorSample sample(int socket) = 0;
};

using SensorAdapterPtr = std::shared_ptr<SensorAdapter>;

class SensorAdapterBase : public SensorAdapter {
public:
    SensorAdapterBase(std::string name,
                      SensorSampleProvider sample_provider,
                      SensorAvailabilityProvider availability_provider = {});

    std::string name() const override;
    bool is_available(int socket) const override;
    SensorSample sample(int socket) override;

protected:
    SensorSampleProvider sample_provider_;
    SensorAvailabilityProvider availability_provider_;

private:
    SensorSample degraded_sample(int socket) const;

    std::string name_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<int, SensorSample> last_good_samples_;
};

class HfiPmtAdapter : public SensorAdapterBase {
public:
    explicit HfiPmtAdapter(SensorSampleProvider sample_provider,
                           SensorAvailabilityProvider availability_provider = {});
};

class AmdHsmpAdapter : public SensorAdapterBase {
public:
    explicit AmdHsmpAdapter(SensorSampleProvider sample_provider,
                            SensorAvailabilityProvider availability_provider = {});
};

class AcpiPmtAdapter : public SensorAdapterBase {
public:
    explicit AcpiPmtAdapter(SensorSampleProvider sample_provider,
                            SensorAvailabilityProvider availability_provider = {});
};

} // namespace telemetry

