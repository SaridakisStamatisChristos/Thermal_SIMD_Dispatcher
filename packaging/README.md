# Packaging Overview

This directory contains example Docker, systemd and Kubernetes packaging for
the standalone dispatcher. Treat the manifests as deployment starting points:
perf-event policy, CPU affinity, thermal sysfs visibility and authentication
must be validated on the target platform.

## Runtime prerequisites

- Linux x86-64 with SSE4.1; AVX2 and AVX-512 are selected only when the host
  and runtime policy permit them.
- OpenSSL runtime libraries.
- `CAP_PERFMON` on Linux 5.9+ or an equivalent `perf_event_open` policy for
  hardware-counter readiness.
- readable package-temperature and frequency sources for strict `/readyz`
  success.

MSR access is optional. The shipped examples do not mount `/dev/cpu`, grant
`CAP_SYS_RAWIO` or use `CAP_SYS_ADMIN`. If a deployment explicitly needs MSR
telemetry, add only the device access and permission required on that host.

## Artifacts

| Path | What it does |
| --- | --- |
| `Dockerfile` | Builds a Debian 12 runtime image containing the executable, controller coefficients and license notices. Its default command runs `--health-check`; supply persistent arguments in an orchestrator. |
| `systemd/thermal-simd.service` | Runs the startup sandbox, then persistent dispatch with `CAP_PERFMON`, systemd hardening and loopback-only metrics. |
| `kubernetes/daemonset.yaml` | Runs the sandbox as an init container, then the persistent dispatcher with `PERFMON` and separate readiness/liveness probes. |
| `kubernetes/service.yaml` | Exposes port 9464 inside the cluster. |
| `kubernetes/networkpolicy.yaml` | Restricts metrics ingress to the monitoring namespace or explicitly labelled pods. |

The example Kubernetes endpoint is plain HTTP. The NetworkPolicy is not a
substitute for encryption or authentication. Configure `--metrics-cert`,
`--metrics-key`, optional mTLS and/or Basic Auth before exposing the endpoint
outside a trusted network boundary; update probe schemes and credentials to
match.

## Configuration

The manifests exercise these implemented options:

- predictive controller: `--temp-ceiling`, `--safety-margin`,
  `--emergency-margin`, `--predictive-alpha`, `--coeff-path`;
- telemetry: `--telemetry-interval`, `--telemetry-max-skew`,
  `--telemetry-ewma`;
- HTTP/StatsD: `--metrics-port`, `--metrics-bind`, `--metrics-cert`,
  `--metrics-key`, `--metrics-ca`, `--metrics-require-client-auth`,
  `--metrics-basic-auth`, `--statsd-host`, `--statsd-port`.

The general environment variables implemented by the executable are
`TSD_LOG_LEVEL` and `TSD_PREDICTIVE_COEFF_PATH`. Other runtime settings should
be passed as command-line arguments or through `--config`. Development-only
variables such as `TSD_FAKE_PERF` must not be used as production configuration.

See [`../docs/configuration.md`](../docs/configuration.md) and
[`../docs/metrics-endpoints.md`](../docs/metrics-endpoints.md) for exact
semantics.

## Validation and release checklist

1. Run the hosted CI, security, sandbox and quality workflows on the exact
   candidate commit.
2. Build the container and execute its health check under the same capability,
   seccomp and sysfs policy planned for production.
3. Validate `/healthz`, `/readyz` and authentication from the actual probe and
   scraper identities.
4. Run the target-aware HIL workflow and retain its per-run evidence artifact.
5. Pin the image by digest. Image signing and registry admission are external
   deployment controls; this repository does not implement or verify them.
