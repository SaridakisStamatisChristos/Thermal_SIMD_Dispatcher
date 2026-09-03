# Metrics and Health Endpoints

The runtime provides an in-process metrics registry, a small HTTP exporter and
an optional StatsD sink. The implementation is in
[`src/observability/metrics.cpp`](../src/observability/metrics.cpp),
[`src/runtime_metrics.c`](../src/runtime_metrics.c) and
[`src/observability/statsd_exporter.cpp`](../src/observability/statsd_exporter.cpp).

## HTTP exporter

The exporter listens on `127.0.0.1:9464` by default. `--metrics-port=0`
disables it. It uses four request workers, a bounded 32-client queue, an 8 KiB
request limit and two-second client I/O timeouts so a stalled client cannot
indefinitely occupy every probe worker.

Only `GET` is supported. Basic authentication, when configured, applies to all
three endpoints:

| Endpoint | Success status | Meaning |
| --- | --- | --- |
| `/metrics` | 200 | Prometheus text snapshot. |
| `/healthz` | 200 while the exporter is running | Process/exporter liveness. Recoverable dependency degradation remains live and is visible in the JSON body. |
| `/readyz` | 200 when strict dependency checks pass; otherwise 503 | Operational readiness for adaptive dispatch. |

Readiness requires controller, telemetry-fusion and perf snapshots newer than
five seconds, no controller fallback, running/non-degraded fusion, and healthy
hardware perf counters. The `/healthz` and `/readyz` bodies contain the same
controller, fusion and perf state; their status-code semantics differ.

Other responses are 401 for failed authentication, 404 for an unknown path, 405
for a non-GET request, and 400/408 for malformed, oversized or timed-out
requests.

## Prometheus metrics

The current `/metrics` payload exposes:

| Metric family | Meaning |
| --- | --- |
| `tsd_patch_transitions_total` | Selection attempts by source width, target width and outcome. |
| `tsd_dwell_time_ms_{sum,count,max}` | Observed dwell durations by SIMD width. |
| `tsd_sensor_health_ratio` | Last reported sensor-health ratio by sensor and socket. |
| `tsd_sensor_quality_ratio` | Last reported sensor-quality ratio by sensor and socket. |
| `tsd_sensor_health_valid` | Whether the last sensor report was valid. |
| `tsd_sensor_health_timestamp_seconds` | UNIX timestamp of the last sensor report. |
| `tsd_package_temperature_c` | Raw safety and filtered control temperatures when available. |
| `tsd_package_temperature_available` | Availability of the raw and filtered temperature channels. |

The C runtime also maintains atomic counters through the public snapshot API in
[`include/thermal/simd/metrics.h`](../include/thermal/simd/metrics.h). Those
counters are available to an embedding application, but they are not currently
duplicated in the Prometheus payload.

## Configuration

| Flag | Description | Default |
| --- | --- | --- |
| `--metrics-port` | HTTP listen port; `0` disables the exporter. | `9464` |
| `--metrics-bind` | Numeric IPv4 bind address. | `127.0.0.1` |
| `--metrics-cert` / `--metrics-key` | PEM certificate and private key; both are required to enable TLS. | disabled |
| `--metrics-ca` | Optional client CA bundle. | unset |
| `--metrics-require-client-auth` | Require a verified client certificate; also requires `--metrics-ca`. | false |
| `--metrics-basic-auth` | Credentials in `user:pass` form. | unset |
| `--statsd-host` | Enables StatsD event export when nonempty. | unset |
| `--statsd-port` | StatsD UDP destination port. | `8125` |

The equivalent JSON keys are documented in
[`docs/configuration.md`](configuration.md). There is no `--metrics-interval`
option and metrics flags are not implicitly mirrored by environment variables.

TLS uses OpenSSL and enforces TLS 1.2 or newer. Supplying a CA without requiring
client authentication loads it for verification configuration but does not by
itself require a client certificate. Bind to a non-loopback address only when
network policy and authentication match the deployment threat model.

## StatsD

StatsD is disabled until a host is configured. Counter and gauge datagrams are
sent immediately when the corresponding transition, dwell or sensor event is
recorded. Each event is one UDP datagram; there is no batching thread or
periodic structured-log flush.

Example names include:

```text
tsd.patch_transition.avx2.avx512.success:1|c
tsd.dwell.observed.avx2:125.000|g
tsd.sensor.package.socket0.health:1.000|g
```

UDP delivery is best effort. A DNS, socket or send failure does not stop the
dispatcher.

## Testing

[`tests/observability/test_metrics_exporter.cpp`](../tests/observability/test_metrics_exporter.cpp)
checks Prometheus formatting, raw/filtered temperature channels, TLS, Basic
Auth, strict readiness versus liveness, StatsD emission and probe availability
while another TLS client stalls.
