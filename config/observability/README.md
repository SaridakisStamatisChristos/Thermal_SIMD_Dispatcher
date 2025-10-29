# Observability Configuration

The `metrics.example.json` file documents how to configure the dispatcher metrics server.

* `bind_address`/`port` configure where the HTTPS listener binds. A port of `0` selects an ephemeral port.
* `tls` enables TLS when `certificate` and `private_key` reference PEM-encoded files. Supplying a `client_ca`
  enables optional client authentication; set `require_client_auth` to `true` to enforce mutual TLS.
* `basic_auth` secures the endpoints using HTTP basic authentication. Populate the `username` and `password` fields
  with values deployed alongside the dispatcher.
* `statsd` enables the StatsD exporter and points it at an upstream aggregator.

Copy the example to your deployment configuration management system and replace the placeholder credential paths.
