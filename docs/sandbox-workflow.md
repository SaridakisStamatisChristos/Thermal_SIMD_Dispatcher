# Sandbox Workflow

This workflow describes how to exercise the dispatcher in a non-production sandbox using synthetic telemetry and workload shims. It underpins developer testing, QA sign-off, and security reviews.

## Goals
- Provide a reproducible environment for stress-testing SIMD patching and thermal controls.
- Simulate adverse telemetry conditions (dropouts, noisy sensors) without touching production hardware.
- Validate packaging artifacts prior to release.

## Components
| Component | Location | Purpose |
| --- | --- | --- |
| Sandbox runner | `ci/sandbox/run.sh` | Orchestrates containers, mounts perf/MSR devices, and collects logs. |
| Synthetic workload | `tests/fixtures/synthetic_workload.so` | Generates CPU-bound loops parameterized by SIMD width. |
| Telemetry fuzzer | `tools/telemetry_fuzzer.py` | Injects jitter, dropouts, and outliers into telemetry stream. |
| Metrics harness | `tools/metrics_probe.py` | Scrapes `/metrics` and `/healthz` during test runs. |

## Setup
1. Build the dispatcher image:
   ```bash
   make sandbox-image
   ```
   This targets `packaging/Dockerfile` with sandbox overlays (installs telemetry fuzzer dependencies and debug symbols).

2. Provision local capabilities:
   ```bash
   sudo modprobe msr
   sudo setcap cap_sys_admin,cap_perfmon+ep ./build/thermal_simd
   ```

3. Launch the sandbox runner:
   ```bash
   ci/sandbox/run.sh --workload synthetic --duration 120 \
     --telemetry-fuzzer tools/telemetry_fuzzer.py \
     --metrics-probe tools/metrics_probe.py \
     --enable-metrics
   ```

## Workflow Details
- The runner starts the dispatcher container with `--health-check` followed by a steady-state workload phase.
- Telemetry fuzzer attaches over a Unix domain socket exposed by the dispatcher (`/run/tsd/telemetry.sock`).
- Metrics probe scrapes `localhost:9753` and writes results to `artifacts/metrics.ndjson`.
- Sandbox artifacts (logs, metrics, telemetry traces) land under `artifacts/YYYYmmdd-HHMMSS/` for upload to CI.

## Scenarios
| Scenario | Flag | Description |
| --- | --- | --- |
| Telemetry dropout | `--telemetry-fuzzer dropout` | Randomly zeroes temperature/frequency for bursts of 500ms. |
| Thermal spike | `--telemetry-fuzzer spike --spike-temp 15` | Injects sudden 15°C spike; expect downgrade and cooldown. |
| Sensor skew | `--telemetry-fuzzer skew --skew-ms 40` | Pushes collector skew beyond `max_skew_ms` to validate staleness guard. |
| Metrics outage | `--disable-metrics` | Ensures dispatcher stays operational without exporters (logs degrade). |

## Exit Criteria
- Dispatcher exits 0.
- `artifacts/*/metrics.ndjson` contains expected counters (`predictive_decisions_total > 0` during spike scenario).
- No `state=emergency` logs during nominal runs.

## Automation
- CI job `ci/sandbox.yml` runs the workflow nightly with dropout and spike scenarios.
- Developers can run `make sandbox-smoke` to execute a shortened (60s) scenario before pushing.
- Security reviews require attaching sandbox artifacts to the compliance ticket (see `docs/security/threat-model.md`).
