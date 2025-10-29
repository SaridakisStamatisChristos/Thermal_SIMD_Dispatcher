#include <telemetry/collector.h>

#include <telemetry/bus.h>

namespace telemetry {

namespace {

TelemetryReading make_reading(double value,
                              bool valid,
                              int quality,
                              std::chrono::steady_clock::time_point now) {
    TelemetryReading reading;
    reading.value = value;
    reading.valid = valid;
    reading.quality = quality;
    reading.timestamp = now;
    return reading;
}

}  // namespace

PeriodicCollector::PeriodicCollector(std::string name, std::chrono::milliseconds interval)
    : name_(std::move(name)), interval_(interval), next_run_(std::chrono::steady_clock::time_point::min()) {}

std::string PeriodicCollector::name() const { return name_; }

void PeriodicCollector::collect(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (now < next_run_) {
        return;
    }
    next_run_ = now + interval_;
    sample(bus, now);
}

PerfCollector::PerfCollector(std::chrono::milliseconds interval, PerfSampleProvider provider, int quality)
    : PeriodicCollector("perf", interval), provider_(std::move(provider)), quality_(quality) {}

void PerfCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) {
        return;
    }
    PerfSample sample = provider_();
    TelemetryReading cpi_reading = make_reading(sample.thermal_cpi, sample.valid, quality_, now);
    TelemetryReading freq_reading = make_reading(sample.freq_hint, sample.valid, quality_, now);
    bus.publish(TelemetrySignal::kThermalCpi, cpi_reading);
    bus.publish(TelemetrySignal::kFrequencyRatio, freq_reading);
}

MsrCollector::MsrCollector(std::chrono::milliseconds interval, TemperatureSampleProvider provider, int quality)
    : PeriodicCollector("msr", interval), provider_(std::move(provider)), quality_(quality) {}

void MsrCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) {
        return;
    }
    TemperatureSample sample = provider_();
    TelemetryReading temp_reading = make_reading(sample.package_temp_c, sample.valid, quality_, now);
    bus.publish(TelemetrySignal::kPackageTempC, temp_reading);
}

RaplCollector::RaplCollector(std::chrono::milliseconds interval, RaplSampleProvider provider, int quality)
    : PeriodicCollector("rapl", interval), provider_(std::move(provider)), quality_(quality) {}

void RaplCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) {
        return;
    }
    RaplSample sample = provider_();
    TelemetryReading reading = make_reading(sample.power_budget_w, sample.valid, quality_, now);
    bus.publish(TelemetrySignal::kPowerBudgetWatts, reading);
}

FreqCollector::FreqCollector(std::chrono::milliseconds interval, PerfSampleProvider provider, int quality)
    : PeriodicCollector("freq", interval), provider_(std::move(provider)), quality_(quality) {}

void FreqCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) {
        return;
    }
    PerfSample sample = provider_();
    TelemetryReading freq_reading = make_reading(sample.freq_hint, sample.valid, quality_, now);
    bus.publish(TelemetrySignal::kFrequencyRatio, freq_reading);
}

OemCollector::OemCollector(std::chrono::milliseconds interval, TemperatureSampleProvider provider, int quality)
    : PeriodicCollector("oem", interval), provider_(std::move(provider)), quality_(quality) {}

void OemCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) {
        return;
    }
    TemperatureSample sample = provider_();
    TelemetryReading temp_reading = make_reading(sample.package_temp_c, sample.valid, quality_, now);
    bus.publish(TelemetrySignal::kPackageTempC, temp_reading);
}

}  // namespace telemetry

