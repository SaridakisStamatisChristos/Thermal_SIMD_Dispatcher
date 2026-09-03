# Runtime Configuration

The dispatcher exposes a small command-line interface and an optional JSON configuration
file to tailor predictive control, telemetry fusion, and observability. CLI flags take
precedence over values loaded from the JSON file.

## Configuration file

Pass `--config=/path/to/runtime.json` to load overrides. The file supports the following
structure:

```json
{
  "predictive": {
    "coeff_path": "config/controller_coeffs.json",
    "temp_ceiling_c": 92,
    "safety_margin_c": 4,
    "emergency_margin_c": 10,
    "alpha": 0.25
  },
  "telemetry": {
    "interval_ms": 50,
    "max_skew_ms": 150,
    "ewma": 0.25
  },
  "metrics": {
    "bind_address": "127.0.0.1",
    "port": 9464,
    "tls": {
      "certificate": "config/certs/dispatcher.crt",
      "private_key": "config/certs/dispatcher.key",
      "client_ca": "config/certs/ca.crt",
      "require_client_auth": false
    },
    "basic_auth": {
      "username": "metrics",
      "password": "change-me"
    },
    "statsd": {
      "host": "127.0.0.1",
      "port": 8125
    }
  }
}
```

All sections are optional—omitted values fall back to the compiled defaults documented
below. The `predictive.coeff_path` defaults to the bundled
`config/controller_coeffs.json` generated alongside the build.

## Key options

| Area | Flag / JSON key | Description | Default |
| ---- | ---------------- | ----------- | ------- |
| Predictive | `--temp-ceiling` / `predictive.temp_ceiling_c` | Controller temperature ceiling in °C. | 92 |
| Predictive | `--safety-margin` / `predictive.safety_margin_c` | Guard band below the ceiling before upgrades. | 4 |
| Predictive | `--emergency-margin` / `predictive.emergency_margin_c` | Additional buffer that forces scalar fallback. | 10 |
| Predictive | `--predictive-alpha` / `predictive.alpha` | CPI EWMA alpha for the predictive controller. | 0.25 |
| Predictive | `--coeff-path` / `predictive.coeff_path` | ARX coefficient bundle path. | `config/controller_coeffs.json` |
| Telemetry | `--telemetry-interval` / `telemetry.interval_ms` | Telemetry fusion poll interval (ms). | 50 |
| Telemetry | `--telemetry-max-skew` / `telemetry.max_skew_ms` | Maximum allowed skew between collectors (ms). | 150 |
| Telemetry | `--telemetry-ewma` / `telemetry.ewma` | Telemetry CPI EWMA alpha. | 0.25 |
| Telemetry | `--telemetry-profile` / `telemetry.profile` | Reserved for a future manifest format. A nonempty value is currently rejected when telemetry fusion starts. | *(unset)* |
| Metrics | `--metrics-port` / `metrics.port` | Prometheus listen port (`0` disables). | 9464 |
| Metrics | `--metrics-bind` / `metrics.bind_address` | Listen address. | `127.0.0.1` |
| Metrics | `--metrics-cert` / `metrics.tls.certificate` | TLS certificate (PEM). | *(unset)* |
| Metrics | `--metrics-key` / `metrics.tls.private_key` | TLS private key (PEM). | *(unset)* |
| Metrics | `--metrics-ca` / `metrics.tls.client_ca` | Optional client CA bundle for mTLS. | *(unset)* |
| Metrics | `--metrics-require-client-auth` / `metrics.tls.require_client_auth` | Enforce client certificates. | `false` |
| Metrics | `--metrics-basic-auth` / `metrics.basic_auth.{username,password}` | HTTP basic auth credentials. | *(unset)* |
| Metrics | `--statsd-host` / `metrics.statsd.host` | StatsD target host. | *(unset)* |
| Metrics | `--statsd-port` / `metrics.statsd.port` | StatsD UDP port. | 8125 |

## Validation rules

- TLS requires both certificate and private key paths. Supplying `--metrics-require-client-auth`
  (or setting `metrics.tls.require_client_auth`) also requires a client CA bundle.
- Basic authentication requires both username and password.
- StatsD is enabled only when both host and port are set.
- Telemetry profile manifests are not implemented. Leave `telemetry.profile`
  unset; a nonempty path is rejected explicitly rather than ignored.
- Telemetry intervals must remain between 10 ms and 60,000 ms; maximum skew
  accepts 0 ms through 60,000 ms.
- Predictive margins must fall between 0 °C and 60 °C.

Invalid combinations terminate the process with a descriptive log entry so that
misconfigurations are caught during startup.
