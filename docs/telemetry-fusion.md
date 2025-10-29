# Telemetry Fusion Architecture

The telemetry fusion layer normalizes and aggregates signals from hardware counters, ACPI sensors, and software probes so that the predictive controller can act on a consistent snapshot.

## Overview
- Lives in `telemetry/fusion.c` with headers under `include/telemetry/fusion.h`.
- Runs on a dedicated thread pinned to an isolated core to prevent dispatcher jitter.
- Publishes a `TelemetrySnapshot` structure to a lock-free ring buffer consumed by the dispatcher loop.

## Data Sources
| Source | Collector | Refresh Interval | Notes |
| --- | --- | --- | --- |
| `perf_event_open` (cycles, instructions) | `perf_collector.c` | Every scheduler tick (50ms default) | Provides CPI and residency data. |
| MSR IA32_THERM_STATUS | `msr_collector.c` | 25ms | Exposes package/core temperatures; falls back to `/sys/class/thermal`. |
| RAPL energy counters | `rapl_collector.c` | 50ms | Derives instantaneous power budget. |
| CPU frequency (`/proc/cpuinfo_cur_freq`) | `freq_collector.c` | 50ms | Captures turbo residency hints. |
| OEM sensors (I2C/PMBus) | `oem_collector.c` | 100ms | Optional but preferred for socket-level thermal accuracy. |

Each collector publishes raw readings into a shared `TelemetryBus`. The fusion layer applies validation, deduplication, and time alignment before publishing.

## Fusion Pipeline
1. **Ingest:** Copy collector updates into a mutable `FusionScratch` struct while checking sequence numbers.
2. **Validate:** Apply per-sensor plausibility checks (`min/max`, delta thresholds). Flag anomalies through `telemetry_anomaly_total`.
3. **Temporal Align:** Convert timestamps to monotonic nanoseconds and ensure readings fall within the current interval window (`±10ms`). Stale readings are tagged with `status=STALE`.
4. **Unit Normalize:** Convert temperatures to °C, power to Watts, frequency to MHz, CPI to dimensionless ratio.
5. **Synthesize Metrics:**
   - Compute `thermal_cpi` as CPI smoothed with an EWMA.
   - Derive `freq_hint` from average frequency vs. nominal base clock.
   - Calculate `power_budget_w` from RAPL delta energy.
6. **Snapshot Publish:** Emit an immutable `TelemetrySnapshot` with `generation` incremented. The dispatcher consumes the snapshot by matching `generation`.

## Error Handling
- Missing mandatory sensors (perf counters, package temperature) mark the snapshot `degraded=true` and trigger scalar fallback.
- Optional sensors populate with `NaN` and log `event=telemetry_optional_missing` to support diagnostics.
- Collector thread crashes escalate through the watchdog, raising `telemetry_watchdog_trip_total` and causing process exit.

## Configuration
Configuration lives in `config/telemetry.toml` and surfaces through CLI/environment overrides:

| Option | CLI Flag | Description | Default |
| --- | --- | --- | --- |
| `poll_interval_ms` | `--telemetry-interval` | Base interval for collectors. | 50 |
| `max_skew_ms` | `--telemetry-max-skew` | Allowed skew between collectors before marking stale. | 15 |
| `ewma_alpha` | `--telemetry-ewma` | EWMA alpha for CPI smoothing. | 0.25 |
| `oem_bus` | `--telemetry-oem-bus` | Path to optional PMBus device. | `/dev/i2c-6` |
| `rapl_domain` | `--telemetry-rapl-domain` | RAPL domain to monitor (e.g., `package-0`). | `package-0` |

Environment variables mirror these flags using the `TSD_TELEMETRY_*` prefix (`TSD_TELEMETRY_INTERVAL`, etc.).

## Observability
The fusion layer emits structured logs with `event=telemetry_snapshot` and includes:
- `generation`
- `degraded`
- `thermal_cpi`
- `temp_package_c`
- `freq_hint`
- `power_budget_w`

Metrics include:
- `telemetry_snapshots_total`
- `telemetry_degraded_total`
- `telemetry_anomaly_total`
- `telemetry_watchdog_trip_total`

See [Metrics Endpoints](metrics-endpoints.md) for export details.

## Testing
- Unit tests under `tests/telemetry/test_telemetry.cpp` mock sensor inputs and verify normalization.
- Hardware-in-the-loop CI job (see [`docs/ci-hil.md`](ci-hil.md)) validates sensor integration on nightly runs when AVX-512 runners tagged `hil` are available.
- `tests/smoke.sh` exercises the telemetry pipeline using the sandbox workflow.
- Coverage ownership for this subsystem is tracked in the [Validation Matrix](testing-matrix.md).
