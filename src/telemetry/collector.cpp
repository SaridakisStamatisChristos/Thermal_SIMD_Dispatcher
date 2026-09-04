#include <telemetry/collector.h>

#include <telemetry/bus.h>

namespace telemetry {

namespace {

TelemetryReading make_reading(double value,
                              bool valid,
                              int quality,
                              std::chrono::steady_clock::time_point now,
                              const char *source) {
    TelemetryReading reading;
    reading.value = value;
    reading.valid = valid;
    reading.quality = quality;
    reading.timestamp = now;
    reading.source = source ? source : "default";
    return reading;
}

}  // namespace

PeriodicCollector::PeriodicCollector(std::string name, std::chrono::milliseconds interval)
    : name_(std::move(name)), interval_(interval), next_run_(std::chrono::steady_clock::time_point::min()) {}

std::string PeriodicCollector::name() const { return name_; }

void PeriodicCollector::collect(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (now < next_run_) return;
    next_run_ = now + interval_;
    sample(bus, now);
}

PerfCollector::PerfCollector(std::chrono::milliseconds interval, PerfSampleProvider provider, int quality)
    : PeriodicCollector("perf", interval), provider_(std::move(provider)), quality_(quality) {}

void PerfCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) return;
    PerfSample sample = provider_();
    bus.publish(TelemetrySignal::kThermalCpi,
                make_reading(sample.thermal_cpi, sample.valid, quality_, now, "perf"));
    bus.publish(TelemetrySignal::kFrequencyRatio,
                make_reading(sample.freq_hint, sample.valid, quality_, now, "perf"));
}

MsrCollector::MsrCollector(std::chrono::milliseconds interval, TemperatureSampleProvider provider, int quality)
    : PeriodicCollector("msr", interval), provider_(std::move(provider)), quality_(quality) {}

void MsrCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) return;
    TemperatureSample sample = provider_();
    bus.publish(TelemetrySignal::kPackageTempC,
                make_reading(sample.package_temp_c, sample.valid, quality_, now, "msr"));
}

RaplCollector::RaplCollector(std::chrono::milliseconds interval, RaplSampleProvider provider, int quality)
    : PeriodicCollector("rapl", interval), provider_(std::move(provider)), quality_(quality) {}

void RaplCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) return;
    RaplSample sample = provider_();
    bus.publish(TelemetrySignal::kPowerBudgetWatts,
                make_reading(sample.power_budget_w, sample.valid, quality_, now, "rapl"));
}

FreqCollector::FreqCollector(std::chrono::milliseconds interval, PerfSampleProvider provider, int quality)
    : PeriodicCollector("freq", interval), provider_(std::move(provider)), quality_(quality) {}

void FreqCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) return;
    PerfSample sample = provider_();
    bus.publish(TelemetrySignal::kFrequencyRatio,
                make_reading(sample.freq_hint, sample.valid, quality_, now, "freq"));
}

OemCollector::OemCollector(std::chrono::milliseconds interval, TemperatureSampleProvider provider, int quality)
    : PeriodicCollector("oem", interval), provider_(std::move(provider)), quality_(quality) {}

void OemCollector::sample(TelemetryBus &bus, std::chrono::steady_clock::time_point now) {
    if (!provider_) return;
    TemperatureSample sample = provider_();
    bus.publish(TelemetrySignal::kPackageTempC,
                make_reading(sample.package_temp_c, sample.valid, quality_, now, "oem"));
}

}  // namespace telemetry
