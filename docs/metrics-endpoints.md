# Metrics Endpoints

The dispatcher exports metrics and health data via a multi-channel strategy tailored for on-host scraping, fleet-wide aggregation, and incident response.

## Architecture
- **In-process registry:** `metrics/registry.c` tracks counters, gauges, and histograms. All subsystems register metrics during initialization.
- **Snapshot API:** `metrics/snapshot.h` exposes `metrics_snapshot_collect()` which produces a read-only view of the current values.
- **Exporters:**
- **Prometheus text endpoint** on `localhost:9464/metrics` (TLS enabled via `--metrics-cert` / `--metrics-key`).
  - **StatsD UDP exporter** (disabled by default) configured via `--statsd-host` and `--statsd-port`.
  - **Structured logs** that emit metric deltas under `event=metrics_flush` for environments without scrape support.

## Prometheus Endpoint
- Implemented in `metrics/prometheus_server.c`.
- Runs on a dedicated thread, using `epoll` and non-blocking I/O to support high-frequency scrapes.
- Supports basic authentication when `--metrics-basic-auth` is provided (format `user:pass`).
- TLS requires pointing `--metrics-cert` and `--metrics-key` at PEM files with read-only permissions.
- Exports default process metrics (RSS, file descriptors) alongside dispatcher-specific gauges.

### Key Metrics
| Metric | Type | Description |
| --- | --- | --- |
| `predictive_forecasts_total` | Counter | Forecast cycles executed by the predictive controller. |
| `predictive_decisions_total` | Counter | Control loop iterations that issued a predictive decision. |
| `predictive_abs_error_millic_total` | Counter | Accumulated absolute error between forecast and observed temperature. |
| `predictive_stale_samples_total` | Counter | Telemetry snapshots rejected because they exceeded the staleness window. |
| `predictive_coeff_reload_total` | Counter | Successful coefficient reloads (startup and SIGHUP). |
| `predictive_coeff_reload_errors_total` | Counter | Failed attempts to reload the coefficient file. |
| `telemetry_snapshots_total` | Counter | Telemetry fusion snapshots published. |
| `telemetry_degraded_total` | Counter | Snapshots flagged as degraded due to missing signals. |
| `patch_transitions_total` | Counter | Successful SIMD trampoline swaps. |
| `patch_failures_total` | Counter | Failed trampoline swaps (auto rollback). |
| `software_timeout_escalations_total` | Counter | Dispatch loop timeouts. |
| `health_check_failures_total` | Counter | Failed health-check runs. |
| `metrics_flush_duration_ms` | Histogram | Latency of exporter flush cycles. |

### Configuration Flags
| Flag | Description | Default |
| --- | --- | --- |
| `--metrics-port` | Listen port for HTTP endpoint. | 9464 |
| `--metrics-addr` | Bind address. | `127.0.0.1` |
| `--metrics-cert` / `--metrics-key` | Enable TLS for Prometheus endpoint. | Disabled |
| `--metrics-ca` | Client CA bundle for mutual TLS. | Disabled |
| `--metrics-require-client-auth` | Enforce mTLS for `/metrics` and `/healthz`. | Disabled |
| `--metrics-basic-auth` | `user:pass` credentials for basic auth. | None |
| `--statsd-host` | StatsD host for UDP export. | Disabled |
| `--statsd-port` | StatsD port. | 8125 |
| `--metrics-interval` | Interval (ms) between StatsD flushes. | 1000 |

Environment variables mirror flags with `TSD_METRICS_*` prefix.

## StatsD Export
- Located in `metrics/statsd_exporter.c`.
- Encodes counters as `metric.name:delta|c`, gauges as `metric.name:value|g`.
- Batches packets up to 512 bytes to respect network MTU and reduce UDP loss.
- Controlled by `--statsd-host`, `--statsd-port`, and `--metrics-interval`.

## Structured Logs
- Enabled via `--log-level=info` or lower.
- Every `metrics_interval` milliseconds, emit `event=metrics_flush` with JSON payload.
- Downstream log shippers (Fluent Bit, Vector) parse and forward to time-series databases.

## Health Probes
- `GET /healthz` (same port as Prometheus) returns 200 when the dispatcher is not degraded and telemetry is fresh.
- Returns 429 when telemetry is stale or predictive controller is in emergency mode.
- Fails closed (503) if patching subsystem reports unrecoverable errors.

## Security Considerations
- Metrics endpoint binds to localhost by default; production deployments front it with mTLS-enabled sidecars.
- Basic auth credentials are read from an environment variable `TSD_METRICS_BASIC_AUTH` when not specified via CLI.
- TLS private keys should be mounted read-only and owned by the service account UID (`sduser`).

## Testing & CI
- `tests/metrics_endpoint_test.cpp` validates Prometheus output formatting.
- CI runs `tests/smoke.sh --metrics` to ensure endpoint readiness.
- `ci/hw-smoke.sh` scrapes the endpoint from a separate node to validate TLS and basic auth wiring.
