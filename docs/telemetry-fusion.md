# Telemetry Fusion Architecture

The telemetry subsystem has two layers:

1. a direct Linux helper for temperature and frequency-ratio signals; and
2. a C++ fusion bus that can combine those direct signals with registered collector providers.

The production bridge deliberately treats an empty fused snapshot as **unavailable**, not as a successful telemetry read. It also exposes package-temperature availability as a safety gate for wider SIMD transitions.

## Implementation map

- `src/telemetry_helper.c` — direct Linux telemetry acquisition, package-aware sensor discovery and recovery/backoff.
- `src/telemetry/bus.cpp` — timestamp/quality-aware signal store.
- `src/telemetry/collector.cpp` — reusable provider-backed collector classes.
- `src/telemetry/fusion.cpp` — polling thread, freshness checks, snapshot generation.
- `src/telemetry/fusion_bridge.cpp` — reference-counted C API, raw safety snapshot, direct-helper fallback/publication boundary, EWMA control normalization and temperature upgrade gate.
- `src/thermal_perf.c` — workload-TID-bound perf-event hardware mode, software fallback and validated hot re-probing.
- `include/telemetry/*.h` — C++ bus, collector and snapshot interfaces.
- `include/thermal/simd/telemetry_fusion.h` — stable C bridge API.

## Direct Linux sources

`tsd_telemetry_helper_t` provides the production hardware path:

- package/CPU temperature selected from package-relevant `/sys/class/thermal/thermal_zone*` and `/sys/class/hwmon/hwmon*` sources;
- package topology from `/sys/devices/system/cpu/cpu*/topology/physical_package_id` when exposed;
- `coretemp`, `k10temp`, `zenpower`, package/Tdie/Tctl-style labels scored ahead of generic CPU-like sensors;
- plausible-range validation before accepting a temperature source;
- APERF/MPERF frequency ratio from `/dev/cpu/<cpu>/msr` when available;
- cpufreq fallback from `/sys/devices/system/cpu/cpu*/cpufreq`;
- exponential retry/backoff for temperature, cpufreq and MSR sources after failures.

The runtime does not assume CPU 0 is available. `thermal_perf.c` reads the caller's affinity mask, records the original workload TID, selects the first allowed CPU for that workload and an additional allowed CPU for the monitor thread when possible. The fusion bridge is started for the selected workload CPU, keeping APERF/MPERF/cpufreq telemetry aligned with the code being controlled. Normal cleanup restores the owner thread's saved affinity mask.

The fusion service is process-wide and reference-counted **for one workload CPU**. Re-acquiring the same CPU increments the reference count. A second context requesting a different CPU is rejected by the singleton and continues with its own CPU-local direct helper rather than silently consuming telemetry from the wrong CPU. Cleanup of one same-CPU context does not stop the polling thread while another user remains.

## Raw safety vs filtered control channels

Temperature and frequency are intentionally represented in two channels at the C bridge boundary:

- **raw** values are the newest direct/fused values suitable for safety decisions;
- **filtered** values are EWMA-smoothed values intended for forecasting/control stability.

Reactive thermal severity, emergency-temperature handling and wider-SIMD authorization consume the raw channel. The predictive policy consumes the filtered channel. A sudden thermal spike therefore reaches the fail-closed guard immediately instead of being delayed by a low-pass filter.

`telemetry_ewma_alpha` controls the filtered channel. `alpha == 0` means bypass filtering and publish the raw value into the filtered channel; it does not freeze the first observation. Missing raw data is represented as unavailable rather than as zero.

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

Temperature is converted from milli-degrees Celsius to degrees Celsius inside the bus. Frequency ratio remains in milli-units end-to-end (for example `875` means `0.875x`). Raw values are retained separately from the bridge's filtered state.

## Collector API

The C++ collector layer provides reusable provider-backed classes:

- `PerfCollector`
- `MsrCollector`
- `RaplCollector`
- `FreqCollector`
- `OemCollector`

These classes do **not** magically discover hardware. A caller must register a collector with a concrete provider. The production runtime guarantees temperature/frequency input through the direct-helper bridge when the host exposes those sources; additional perf, RAPL or OEM providers can be registered by integrations that have those data sources.

`PerfSample::freq_hint` is defined in frequency-ratio milli-units (`1000 == 1.0x`), not MHz.

## Snapshot generation and runtime configuration

`TelemetryFusion` owns a polling thread. In the full runtime, the bridge consumes the configured values instead of hard-coded duplicates:

- `telemetry_interval_ms` controls the poll interval;
- `telemetry_max_skew_ms` controls the freshness window;
- `telemetry_ewma_alpha` controls filtered control telemetry;
- snapshot ring capacity remains 128.

When the bridge is used directly before the global runtime configuration has been initialized, it preserves compatibility defaults rather than confusing zero-initialized storage with an explicit runtime configuration.

`telemetry_profile_path` is **not** implemented as a profile-manifest parser. A non-empty profile is rejected explicitly at fusion startup rather than silently accepted as a no-op; the owning perf context retains its CPU-local direct telemetry fallback.

The snapshot ring is synchronized with a mutex and condition variable. It is not lock-free.

Each generation may contain:

- package temperature;
- frequency ratio;
- thermal CPI;
- power budget.

The production fusion layer marks a snapshot degraded when either temperature or frequency ratio is missing. CPI remains authoritative in `thermal_perf.c`, and power is optional enrichment, so their absence alone does not mark an otherwise valid thermal snapshot degraded.

`tsd_telemetry_fusion_sample()` returns success only when at least one temperature/frequency signal is usable. Empty snapshots return `-1`, allowing callers to use their direct source instead of silently consuming zero-valued telemetry.

## Fail-closed temperature gate

Once fusion is running, raw package temperature becomes an explicit authorization signal for wider SIMD widths:

- a valid raw temperature sample permits consideration by normal transition policy;
- loss of raw temperature immediately selects SSE4.1 if a wider slot is active;
- the central runtime transition gate denies later non-SSE authorization while temperature remains unavailable;
- the configured safety margin blocks upgrades too close to the temperature ceiling;
- the configured emergency margin can force the conservative SSE4.1 path using the unsmoothed raw value;
- downgrades and the SSE4.1 fail-safe path remain available;
- recovery of valid temperature headroom re-opens the transition gate.

Before fusion starts the gate is permissive so the legacy startup sequence remains API-compatible; fusion establishes the real thermal gate immediately when the perf runtime is initialized.

## Perf-event failure and recovery

Hardware perf counters are not treated as permanently available after startup. The primary cycle/instruction group is bound to the recorded **workload thread TID** both initially and during hot recovery; a monitor-thread reprobe cannot accidentally attach replacement events to itself.

If the primary group cannot be read, returns an invalid layout, or repeatedly stops accumulating running time, the runtime:

1. closes the invalid perf descriptors;
2. enters software/degraded mode;
3. fails closed to SSE4.1 by default;
4. continuously denies wider SIMD while software mode remains active unless `TSD_ALLOW_SOFTWARE_UPGRADES` is explicitly enabled;
5. publishes unhealthy perf state for readiness;
6. schedules a hardware re-probe.

Re-probing uses bounded exponential backoff beginning at five seconds and capped at sixty seconds. Recovery is transactional from the observable runtime's perspective. A candidate replacement group must:

1. open the cycle leader and instruction member against the original workload TID;
2. successfully RESET/ENABLE the complete group;
3. establish two valid group observations;
4. demonstrate increasing enabled time and running time;
5. demonstrate cycle and instruction progress;
6. build a fresh baseline.

Only after those checks succeed is the mode published as `HARDWARE`, degraded policy exited and the recovery metric incremented. A failed validation leaves the runtime in software/degraded mode and schedules another attempt. Counter scaling rejects zero enabled/running time instead of treating an unscaled zero-runtime delta as healthy data.

The optional LLC-miss counter has its own independent re-probe schedule. Losing only that event temporarily disables the memory-bound guard without forcing the primary cycle/instruction group out of hardware mode; successful LLC reopening seeds a fresh counter value before the guard resumes.

Initial successful startup is not counted as a recovery; the recovery metric is reserved for a validated software-to-hardware transition.

The `TSD_FAKE_PERF` test/development override intentionally remains software-only and does not hot-reprobe real counters.

## Runtime interaction, liveness and readiness

`thermal_perf.c` first asks the fusion bridge for temperature/frequency telemetry when that context acquired the process-wide fusion service. Otherwise it uses the context's CPU-local direct helper. With the production bridge behavior above:

- fresh raw and filtered channels are returned together when present;
- the bridge continuously seeds its bus from the direct Linux helper;
- if fusion is unavailable, rejected by configuration, or owned by another CPU, the per-context direct helper remains authoritative.

CPI itself is measured by the perf/software adaptation layer in `thermal_perf.c`; it is not fabricated by the fusion bridge.

Perf mode and primary-counter health are published separately from fusion state. The readiness aggregation treats software/degraded perf mode as not ready even when temperature/frequency fusion remains alive. The controller publishes a heartbeat every monitor tick, including periods where no width change is recommended, so a stable healthy dispatcher does not become falsely stale.

`/healthz` is liveness only: a recoverable dependency loss does not ask the orchestrator to kill a process that is actively re-probing. `/readyz` is strict runtime readiness and requires fresh controller/fusion state plus healthy hardware perf counters. The Kubernetes example uses those same semantics.

## Recovery semantics

The direct helper has explicit retry state for:

- temperature-source discovery;
- cpufreq paths;
- MSR reopening.

Direct sensor backoff begins at 5 seconds and grows to 600 seconds. Primary perf-event and optional LLC re-probe backoff are separately capped at 60 seconds. Recovery metrics for the primary perf mode are emitted only after a validated hardware baseline succeeds.

The fusion thread itself has no separate crash watchdog at present. Process/service supervision remains the responsibility of the executable's deployment layer.

## Tests

Relevant automated coverage includes:

- `tests/telemetry/test_telemetry.cpp` — sensor adapter behavior;
- `tests/telemetry/test_fusion_thread.cpp` — collector/freshness behavior, production C bridge publication, raw-spike safety visibility and EWMA-bypass semantics;
- `tests/telemetry/test_fusion_stress.cpp` — concurrent fusion stress;
- `tests/perf/test_perf_resilience.c` — runtime group-loss fail-closed behavior, continuous software upgrade authorization, real group-progress validation, allowed-cpuset CPU selection, affinity restoration and CPU-coherent fusion reference counting;
- `tests/stress/telemetry_faults.c` — degraded/recovery fault scenarios;
- `tests/observability/test_metrics_exporter.cpp` — strict readiness, liveness separation and slow-client availability;
- standard CTest smoke registrations for the stress binaries;
- `.github/workflows/sandbox.yml` — software-perf degraded mode;
- `.github/workflows/quality.yml` — GCC/Clang, ASan+UBSan, focused TSan concurrency, installed-consumer, Make/install and container gates;
- `.github/workflows/hil.yml` — self-hosted hardware characterization with retained time-series artifacts.

Hardware-specific validation remains separate because hosted CI cannot guarantee AVX-512, perf permissions, MSR access, RAPL access or repeatable thermal conditions.
