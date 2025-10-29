# Packaging Overview

This directory contains deployable artifacts for Docker, systemd, and Kubernetes targets. Packaging is designed to honor the dispatcher attestation requirements and expose new configuration knobs introduced with the predictive controller, telemetry fusion, and metrics endpoints.

## Prerequisites
- Hosts must provide `CAP_PERFMON` (Linux 5.9+) and readable `/dev/cpu/*/msr` devices.
- Kernel parameter `kernel.perf_event_paranoid` must be `<=1`.
- TLS materials for metrics endpoint stored under `/etc/tsd/tls/` (readable by service account `sduser`).
- Attestation bundle (`patcher_measurement.json`, `attestor_pub.pem`) deployed via secrets management.
- ConfigMap/Env config for telemetry (`TSD_TELEMETRY_*`), predictive controller (`TSD_PREDICTIVE_*`), and metrics (`TSD_METRICS_*`).

## Artifacts
| Path | Description |
| --- | --- |
| `Dockerfile` | Multi-stage build producing runtime image with sandbox hooks and attestation client. |
| `systemd/thermal-simd.service` | Hardened unit with AmbientCapabilities set for perf/MSR access, `ProtectKernelModules=yes`. |
| `kubernetes/daemonset.yaml` | DaemonSet with hostPath mounts for MSR, perf, and attestation materials; exposes metrics via sidecar. |
| `kubernetes/networkpolicy.yaml` | Restricts ingress to metrics port and attestation service. |
| `kubernetes/service.yaml` | ClusterIP service for metrics scraping (mTLS required). |

## Configuration Knobs
- **Predictive Controller:** `--temp-ceiling`, `--safety-margin`, `--emergency-margin`, `--predictive-alpha`, `--coeff-path`.
- **Telemetry Fusion:** `--telemetry-interval`, `--telemetry-max-skew`, `--telemetry-ewma`, `--telemetry-oem-bus`, `--telemetry-rapl-domain`.
- **Metrics:** `--metrics-port`, `--metrics-addr`, `--metrics-cert`, `--metrics-key`, `--metrics-basic-auth`, `--statsd-host`, `--statsd-port`, `--metrics-interval`.

Each packaging target surfaces these knobs through environment variables or command-line args. See comments in the respective manifests for wiring examples.

## CI Expectations
- `ci/hw-smoke.sh` builds the Docker image and runs attestation verification on hardware nightly.
- `ci/sandbox.yml` executes sandbox workflow to validate telemetry fuzzer compatibility.
- `ci/security.yml` enforces cosign signature checks before publishing images.

## Release Checklist
1. Update attestation bundle hashes in `packaging/artifacts/README.md` (create if missing).
2. Bump ConfigMap default coefficients to match release.
3. Run `make sandbox-smoke` and attach artifacts to release ticket.
4. Tag Docker image with release version and cosign sign it.
5. Verify metrics endpoint TLS by running `tools/metrics_probe.py --verify-tls` against staging cluster.
