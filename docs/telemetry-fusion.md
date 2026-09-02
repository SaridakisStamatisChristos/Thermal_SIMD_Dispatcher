# Telemetry Fusion Architecture

The telemetry subsystem has two layers:

1. a direct Linux helper for temperature and frequency-ratio signals; and
2. a C++ fusion bus that can combine those direct signals with registered collector providers.

The production bridge deliberately treats an empty fused snapshot as **unavailable**, not as a successful telemetry read. It also exposes package-temperature availability as a safety gate for wider SIMD transitions.

## Implementation map

- `src/telemetry_helper.c` — direct Linux telemetry acquisition and recovery/backoff.
- `src/telemetry/bus.cpp` — timestamp/quality-aware signal store.
- `src/telemetry/collector.cpp` — reusable provider-backed collector classes.
- `src/telemetry/fusion.cpp` — polling thread, freshness checks, snapshot generation.
- `src/telemetry/fusion_bridge.cpp` — reference-counted C API, direct-helper fallback/publication boundary and temperature upgrade gate.
- `src/thermal_perf.c` — perf-event hardware mode, software fallback and hot re-probing.
- `include/telemetry/*.h` — C++ bus, collector and snapshot interfaces.
- `include/thermal/simd/telemetry_fusion.h` — stable C bridge API.

## Direct Linux sources

`tsd_telemetry_helper_t` currently provides the production hardware path:

- package/CPU temperature from readable `/sys/class/thermal/thermal_zone*/temp` entries;
- APERF/MPERF frequency ratio from `/dev/cpu/<cpu>/msr` when available;
- cpufreq fallback from `/sys/devices/system/cpu/cpu*/cpufreq`;
- exponential retry/backoff for temperature, cpufreq and MSR sources after failures.

The runtime no longer assumes CPU 0 is available. `thermal_perf.c` reads the process affinity mask, selects the first allowed CPU for the workload and an additional allowed CPU for the monitor thread when possible. The fusion bridge is started for the selected workload CPU, keeping APERF/MPERF/cpufreq telemetry aligned with the code being controlled.

The fusion service is process-wide and reference-counted. Multiple perf contexts may acquire it; cleanup of one context does not stop the polling thread while another user remains.

## Fusion bus

`TelemetryBus` stores the newest/best reading per signal. A reading contains:

- value;
- validity;
- quality;
- monotonic timestamp.

Collector registration and bus-manager access are mutex-protected. `poll()` takes a stable copy of the current bus/collector set before invoking providers, so collectors may be registered without racing the polling loop.

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

These classes do **not** magically discover hardware. A caller must register a collector with a concrete provider. The production runtime guarantees temperature/frequency input through the direct-helper bridge when the host exposes those sources; additional perf, RAPL or OEM providers can be registered by integrations that have those data sources.

`PerfSample::freq_hint` is defined in frequency-ratio milli-units (`1000 == 1.0x`), not MHz.

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

The production fusion layer marks a snapshot degraded when either temperature or frequency ratio is missing. CPI remains authoritative in `thermal_perf.c`, and power is optional enrichment, so their absence alone does not mark an otherwise valid thermal snapshot degraded.

`tsd_telemetry_fusion_sample()` returns success only when at least one temperature/frequency signal is usable. Empty snapshots return `-1`, allowing callers to use their direct source instead of silently consuming zero-valued telemetry.

## Fail-closed temperature gate

Once fusion is running, package temperature becomes an explicit authorization signal for wider SIMD widths:

- a valid temperature sample permits normal transition policy;
- loss of temperature immediately selects SSE4.1 if a wider slot is active;
- the central runtime transition gate denies later non-SSE authorization while temperature remains unavailable;
- downgrades and the SSE4.1 fail-safe path remain available;
- recovery of a valid temperature sample re-opens the transition gate.

Before fusion starts the gate is permissive so the legacy startup sequence remains API-compatible; fusion establishes the real thermal gate immediately when the perf runtime is initialized.

## Perf-event failure and recovery

Hardware perf counters are no longer treated as permanently available after startup. If the primary cycle/instruction group cannot be read or returns an invalid layout, the runtime:

1. closes the invalid perf descriptors;
2. enters software/degraded mode;
3. fails closed to SSE4.1 by default;
4. publishes unhealthy perf state for readiness;
5. schedules a hardware re-probe.

Re-probing uses bounded exponential backoff beginning at five seconds and capped at sixty seconds. A successful re-open enables the counters and establishes a fresh hardware baseline before normal hardware evaluation resumes. Initial successful startup is not counted as a recovery; the recovery metric is reserved for a software-to-hardware transition.

The `TSD_FAKE_PERF` test/development override intentionally remains software-only and does not hot-reprobe real counters.

## Runtime interaction and readiness

`thermal_perf.c` first asks the fusion bridge for temperature/frequency telemetry. With the production bridge behavior above:

- a fresh fused temperature/frequency snapshot is returned when present;
- otherwise the bridge samples the direct Linux helper and seeds the bus;
- if the bridge cannot obtain a usable signal, the existing per-context direct helper fallback remains available.

CPI itself is measured by the perf/software adaptation layer in `thermal_perf.c`; it is not fabricated by the fusion bridge.

Perf mode and primary-counter health are published into observability. The readiness aggregation treats software/degraded perf mode as not ready even when the temperature/frequency fusion thread is otherwise alive. This prevents `/readyz` from advertising full adaptive capability after hardware-counter loss.

## Recovery semantics

The direct helper has explicit retry state for:

- thermal-zone discovery;
- cpufreq paths;
- MSR reopening.

Direct sensor backoff begins at 5 seconds and grows to 600 seconds. Perf-event re-probe backoff is separately capped at 60 seconds. Recovery metrics are emitted only after the corresponding source becomes usable again.

The fusion thread itself has no separate crash watchdog at present. If watchdog supervision is required, use the service manager/container orchestrator or add an explicit runtime watchdog rather than assuming one exists.

## Tests

Relevant automated coverage includes:

- `tests/telemetry/test_telemetry.cpp` — sensor adapter behavior;
- `tests/telemetry/test_fusion_thread.cpp` — collector/freshness behavior plus the production C bridge publication path;
- `tests/telemetry/test_fusion_stress.cpp` — concurrent fusion stress;
- `tests/perf/test_perf_resilience.c` — runtime group-loss fail-closed behavior, allowed-cpuset CPU selection and fusion reference counting;
- `tests/stress/telemetry_faults.c` — degraded/recovery fault scenarios;
- `tests/observability/test_metrics_exporter.cpp` — degraded perf state makes readiness fail;
- standard CTest smoke registrations for the stress binaries;
- `.github/workflows/sandbox.yml` — software-perf degraded mode;
- `.github/workflows/quality.yml` — GCC/Clang, sanitizer and packaging gates.

Hardware-specific validation remains separate because hosted CI cannot guarantee AVX-512, perf permissions, MSR access or repeatable thermal conditions.
