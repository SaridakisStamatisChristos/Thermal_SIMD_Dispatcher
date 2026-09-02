#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <telemetry/bus.h>

namespace telemetry {

struct PerfSample {
    double thermal_cpi = 0.0;
    /* Frequency ratio in milli-units: 1000 == 1.0x nominal/reference. */
    double freq_hint = 0.0;
    bool valid = false;
};

struct TemperatureSample {
    /* Degrees Celsius. */
    double package_temp_c = 0.0;
    bool valid = false;
};

struct RaplSample {
    /* Watts. */
    double power_budget_w = 0.0;
    bool valid = false;
};

using PerfSampleProvider = std::function<PerfSample()>;
using TemperatureSampleProvider = std::function<TemperatureSample()>;
using RaplSampleProvider = std::function<RaplSample()>;

class TelemetryCollector {
public:
    virtual ~TelemetryCollector() = default;

    virtual std::string name() const = 0;
    virtual void collect(TelemetryBus &bus, std::chrono::steady_clock::time_point now) = 0;
};

class PeriodicCollector : public TelemetryCollector {
public:
    PeriodicCollector(std::string name, std::chrono::milliseconds interval);

    std::string name() const override;
    void collect(TelemetryBus &bus, std::chrono::steady_clock::time_point now) override;

protected:
    virtual void sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) = 0;

private:
    std::string name_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point next_run_;
};

class PerfCollector : public PeriodicCollector {
public:
    PerfCollector(std::chrono::milliseconds interval, PerfSampleProvider provider, int quality = 0);

protected:
    void sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) override;

private:
    PerfSampleProvider provider_;
    int quality_;
};

class MsrCollector : public PeriodicCollector {
public:
    MsrCollector(std::chrono::milliseconds interval, TemperatureSampleProvider provider, int quality = 0);

protected:
    void sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) override;

private:
    TemperatureSampleProvider provider_;
    int quality_;
};

class RaplCollector : public PeriodicCollector {
public:
    RaplCollector(std::chrono::milliseconds interval, RaplSampleProvider provider, int quality = 0);

protected:
    void sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) override;

private:
    RaplSampleProvider provider_;
    int quality_;
};

class FreqCollector : public PeriodicCollector {
public:
    FreqCollector(std::chrono::milliseconds interval, PerfSampleProvider provider, int quality = 0);

protected:
    void sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) override;

private:
    PerfSampleProvider provider_;
    int quality_;
};

class OemCollector : public PeriodicCollector {
public:
    OemCollector(std::chrono::milliseconds interval, TemperatureSampleProvider provider, int quality = 0);

protected:
    void sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) override;

private:
    TemperatureSampleProvider provider_;
    int quality_;
};

using TelemetryCollectorPtr = std::shared_ptr<TelemetryCollector>;
using TelemetryCollectorList = std::vector<TelemetryCollectorPtr>;

}  // namespace telemetry
