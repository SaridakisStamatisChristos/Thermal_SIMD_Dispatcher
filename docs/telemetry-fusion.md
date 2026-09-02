# Telemetry Fusion Architecture

The telemetry subsystem has two layers:

1. a direct Linux helper for temperature and frequency-ratio signals; and
2. a C++ fusion bus that can combine those direct signals with registered collector providers.

The production bridge deliberately treats an empty fused snapshot as **unavailable**, not as a successful telemetry read. This prevents the fusion layer from suppressing valid direct hardware telemetry.

## Implementation map

- `src/telemetry_helper.c` — direct Linux telemetry acquisition and recovery/backoff.
- `src/telemetry/bus.cpp` — timestamp/quality-aware signal store.
- `src/telemetry/collector.cpp` — reusable provider-backed collector classes.
- `src/telemetry/fusion.cpp` — polling thread, freshness checks, snapshot generation.
- `src/telemetry/fusion_bridge.cpp` — C API used by the runtime and direct-helper fallback/publication boundary.
- `include/telemetry/*.h` — C++ bus, collector and snapshot interfaces.
- `include/thermal/simd/telemetry_fusion.h` — stable C bridge API.

## Direct Linux sources

`tsd_telemetry_helper_t` currently provides the production hardware path:

- package/CPU temperature from readable `/sys/class/thermal/thermal_zone*/temp` entries;
- APERF/MPERF frequency ratio from `/dev/cpu/<cpu>/msr` when available;
- cpufreq fallback from `/sys/devices/system/cpu/cpu*/cpufreq`;
- exponential retry/backoff for temperature, cpufreq and MSR sources after failures.

The bridge initializes a helper for CPU 0, which is also the dispatcher workload CPU in the current runtime. When fusion has not yet produced a usable temperature/frequency snapshot, the bridge samples this helper directly, publishes the available values into the fusion bus, and returns the direct sample to the caller.

## Fusion bus

`TelemetryBus` stores the newest/best reading per signal. A reading contains:

- value;
- validity;
- quality;
- monotonic timestamp.

A newly published reading replaces the current one when it has at least the current quality or a newer timestamp.

The production C boundary can publish already-normalized direct samples with:

```c
int tsd_telemetry_fusion_publish_sample(const tsd_telemetry_sample_t *sample);
```

Temperature is converted from milli-degrees Celsius to degrees Celsius inside the bus. Frequency ratio remains in milli-units end-to-end (for example `875` means `0.875x`).

## Collector API

The C++ collector layer provides reusable provider-backed classes:

- `PerfCollector`
- `MsrCollector`
- `RaplCollector`
- `FreqCollector`
- `OemCollector`

These classes do **not** magically discover hardware. A caller must register a collector with a concrete provider. The production runtime currently guarantees temperature/frequency input through the direct-helper bridge; additional perf, RAPL or OEM providers can be registered by integrations that have those data sources.

## Snapshot generation

`TelemetryFusion` owns a polling thread. The production bridge uses:

- poll interval: 50 ms;
- freshness window: 150 ms;
- snapshot ring capacity: 128.

The snapshot ring is synchronized with a mutex and condition variable. It is not lock-free.

Each generation may contain:

- package temperature;
- frequency ratio;
- thermal CPI;
- power budget.

A snapshot is marked degraded when temperature, frequency or CPI is missing. Power is currently optional for the degraded flag.

`tsd_telemetry_fusion_sample()` returns success only when at least one temperature/frequency signal is usable. Empty snapshots return `-1`, allowing callers to use their direct source instead of silently consuming zero-valued telemetry.

## Runtime interaction

`thermal_perf.c` first asks the fusion bridge for temperature/frequency telemetry. With the production bridge behavior above:

- a fresh fused temperature/frequency snapshot is returned when present;
- otherwise the bridge samples the direct Linux helper and seeds the bus;
- if the bridge cannot obtain a usable signal, the existing `thermal_perf.c` helper fallback remains available.

CPI itself is still measured by the perf/software adaptation layer in `thermal_perf.c`; it is not fabricated by the fusion bridge.

## Recovery semantics

The direct helper has explicit retry state for:

- thermal-zone discovery;
- cpufreq paths;
- MSR reopening.

Backoff begins at 5 seconds and grows to 600 seconds. Recovery metrics are emitted only after a source becomes usable again.

The fusion thread itself has no separate crash watchdog at present. If watchdog supervision is required, use the service manager/container orchestrator or add an explicit runtime watchdog rather than assuming one exists.

## Tests

Relevant automated coverage includes:

- `tests/telemetry/test_telemetry.cpp` — sensor adapter behavior;
- `tests/telemetry/test_fusion_thread.cpp` — collector/freshness behavior plus the production C bridge publication path;
- `tests/telemetry/test_fusion_stress.cpp` — concurrent fusion stress;
- `tests/stress/telemetry_faults.c` — degraded/recovery fault scenarios;
- standard CTest smoke registrations for the stress binaries;
- `.github/workflows/sandbox.yml` — software-perf degraded mode;
- `.github/workflows/quality.yml` — GCC/Clang, sanitizer and packaging gates.

Hardware-specific validation remains separate because hosted CI cannot guarantee AVX-512, perf permissions, MSR access or repeatable thermal conditions.
