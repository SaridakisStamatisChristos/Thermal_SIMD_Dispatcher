# Telemetry Subsystem Overview

The telemetry subsystem normalizes hardware sensor readings, fuses them into
consistent snapshots, and publishes those frames to the predictive controller.
This document describes the new collector abstractions, the telemetry bus
manager, and the fusion thread that feeds controller consumers.

## Module Layout

The subsystem lives under `src/telemetry/` with public headers in
`include/telemetry/`:

| Path | Description |
| ---- | ----------- |
| `include/telemetry/bus.h` | Shared `TelemetryBus` state and manager facade. |
| `include/telemetry/collector.h` | Collector interfaces plus concrete collectors for perf, MSR, RAPL, frequency, and OEM sources. |
| `include/telemetry/fusion.h` | Ring buffer, fusion thread, and snapshot types. |
| `src/telemetry/bus.cpp` | `TelemetryBus` and manager implementation. |
| `src/telemetry/collector.cpp` | Periodic collector helpers and concrete collectors. |
| `src/telemetry/fusion.cpp` | Fusion thread, snapshot ring buffer, and freshness logic. |
| `src/telemetry/fusion_bridge.cpp` | C bridge that exposes fusion outputs to the legacy runtime. |

The bridge exports the `tsd_telemetry_fusion_*` C helpers declared in
`include/thermal/simd/telemetry_fusion.h` so C modules can start/stop the fusion
thread and fetch fused telemetry samples.

## Collectors & Bus Manager

Each collector implements the `TelemetryCollector` interface and publishes
`TelemetryReading` values onto the shared `TelemetryBus`:

* `PerfCollector` – provides CPI and frequency ratio hints derived from perf
  counters.
* `MsrCollector` – ingests package/core temperatures from IA32 thermal MSRs.
* `RaplCollector` – translates RAPL energy deltas into instantaneous power
  budgets.
* `FreqCollector` – polls `/proc/cpuinfo_cur_freq` style interfaces for turbo
  residency hints.
* `OemCollector` – optionally sources OEM/PMBus temperatures with higher
  quality scores.

`TelemetryBusManager` owns the collector list and coordinates polling on the
fusion thread. Collectors are `PeriodicCollector` derivatives that enforce their
own polling cadence while protecting the bus from concurrent updates.

## Fusion Thread & Snapshots

`TelemetryFusion` owns a background thread that polls registered collectors,
aggregates their latest readings, and emits immutable `TelemetrySnapshot`
instances into a lock-protected ring buffer. Freshness is enforced with a
configurable window (`TelemetryFusionConfig::freshness_window`); any signal that
misses the window is marked unavailable and the snapshot is flagged as
`degraded` so the controller can fall back.

Consumers can either call `latest_snapshot()` for a non-blocking read or
`wait_for_snapshot()` to block on a minimum generation. The ring buffer size is
configurable to cover fast polling cadences without blocking producers.

## Controller Integration

`tsd_perf_init()` now starts the fusion thread via
`tsd_telemetry_fusion_start()`, and `tsd_perf_cleanup()` stops it. During perf
evaluations the runtime first tries `tsd_telemetry_fusion_sample()` to obtain a
fused frame before falling back to the legacy synchronous helper. This keeps the
predictive controller aligned with fused telemetry while preserving existing
fallback behaviour when collectors are unavailable.

## Testing

Two new test binaries exercise the subsystem:

* `test_telemetry_fusion` – integration test that verifies collectors publish to
  the bus, snapshots respect freshness, and degradation flags propagate.
* `test_telemetry_fusion_stress` – stress harness that ensures the ring buffer
  and fusion thread remain responsive over rapid polling cycles.

Both tests live under `tests/telemetry/` and are wired into CTest.

