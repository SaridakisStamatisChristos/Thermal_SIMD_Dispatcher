# Thermal‑Aware Self‑Patching SIMD Dispatcher

Production‑grade, Linux x86‑64 only. Runtime chooses between SSE4.1 / AVX2 / AVX‑512 (XMM‑only) and self‑patches a tiny trampoline under strict W^X with a double buffer. Thermal adaptation uses time‑scaled CPI from `perf_event_open` with hysteresis, cooldown and a minimum dwell time. A small shim handles scalar↔SIMD and avoids AVX/SSE transition penalties.

## Build

### Make
```bash
make
```

### CMake
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

## Run

> Requires `CAP_PERFMON` or `sudo sysctl kernel.perf_event_paranoid=0`.

```bash
./thermal_simd --help
./thermal_simd --no-avx512 --interval=100 --down-ratio=1.3 --duration-sec=5
```

## Flags
- `--interval=MS` check interval (default 50)
- `--down-count=N` throttles before downgrade (default 3)
- `--up-count=N` stable intervals before upgrade (default 5)
- `--down-ratio=R` throttle threshold as CPI multiple (default 1.5)
- `--cooldown-down=MS` cooldown after downgrade (default 1000)
- `--cooldown-up=MS` cooldown after upgrade (default 2000)
- `--min-dwell=MS` minimum time per SIMD width (default 200)
- `--no-avx512` disable AVX‑512 usage
- `--duration-sec=S` runtime duration for demo (default 10)
- `--work-iters=N` inner work iterations per tick (default 10,000,000)
- `--degraded-timeout-sec=S` fail closed if hardware counters remain unavailable for S seconds (default 120)
- `--health-check` run diagnostics (perf counters, telemetry, trampolines) and exit with status
- `--log-level=LEVEL` set log verbosity (`error`, `warn`, `info`, `debug`; default `info`)

Environment override:
- `TSD_LOG_LEVEL` mirrors `--log-level` for non-interactive deployments.

## Health Check

The dispatcher exposes a one-shot diagnostic mode that validates hardware counters, telemetry probes, and trampoline integrity before workloads start:

```bash
./thermal_simd --health-check
```

The command exits non-zero when the dispatcher would operate in degraded mode (e.g. missing `perf_event_open` permissions or inaccessible MSRs) and increments the `health_check_failures` metric.

## Metrics & Observability

Structured log lines (key=value) and in-process counters provide hooks for Prometheus/StatsD scraping. The following counters are tracked in `runtime_metrics.c` and exposed via log snapshots:

- `perf_fallbacks` / `perf_recoveries`
- `telemetry_temp_*`, `telemetry_freq_*`, `telemetry_msr_*`
- `patch_transitions` / `patch_failures`
- `software_timeout_escalations`
- `health_check_failures`

Sensor dropouts automatically trigger exponential back-off retries and emit logs such as `event=telemetry_sensor state=degraded sensor=temp` to simplify alert wiring.

## Tests
Run smoke tests (build + basic run):
```bash
tests/compile.sh && tests/smoke.sh
```

A hardware-backed nightly can re-use the new helper script:

```bash
ci/hw-smoke.sh
```

## Packaging

- `packaging/Dockerfile` builds a minimal container with the dispatcher defaulting to health checks on startup.
- `packaging/systemd/thermal-simd.service` is a hardened unit file that runs the binary with the required capabilities.
- `packaging/kubernetes/daemonset.yaml` demonstrates a daemonset with MSR/perf mounts and capability grants.

## Notes
- Requires SSE4.1 (fails fast otherwise)
- Uses `perf_event_open`; in containers, add `--cap-add=SYS_ADMIN` or run privileged
- XMM‑only payloads to minimize downclocks and power
- Patch failures restore trampoline page protections before retrying so the runtime fails closed

## License
This project is distributed under a proprietary commercial license. See [LICENSE](LICENSE) for full terms.
