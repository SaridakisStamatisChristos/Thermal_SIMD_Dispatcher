# Thermal-Aware SIMD Dispatcher

Linux x86-64 runtime that selects between SSE4.1, AVX2 and AVX-512 implementations using performance-counter and thermal telemetry.

The dispatcher uses an **immutable W^X code table**: each implementation is written once while the backing page is `RW` and non-executable, the page is sealed `RX`, and runtime transitions only atomically select an already-sealed slot. No executable page is rewritten during normal operation. The built-in demonstration payloads execute genuine 128-bit, 256-bit and 512-bit vector operations while preserving the legacy scalar shim ABI.

Thermal adaptation combines time-scaled CPI from `perf_event_open`, LLC-miss information, temperature/frequency telemetry, hysteresis, cooldown and minimum dwell. A model-assisted predictive policy can score candidate widths from recent telemetry and an ARX temperature forecast; if the predictive path is unavailable, the runtime falls back to the conservative hysteresis controller.

## Platform and prerequisites

- Linux 5.9+ on x86-64.
- SSE4.1 is the minimum supported ISA.
- `CAP_PERFMON` or equivalent `perf_event_open` permission for hardware-counter mode.
- Optional `/dev/cpu/*/msr` access for MSR-backed telemetry.
- OpenSSL development/runtime libraries for metrics TLS and trampoline attestation hashing.
- Optional metrics TLS materials when exposing `/metrics` off-host.
- Optional controller coefficient overrides through `TSD_PREDICTIVE_*` / `TSD_TELEMETRY_*` environment variables.

The runtime degrades safely when hardware counters or telemetry are unavailable. By default software-only mode does not upgrade above SSE4.1 unless explicitly allowed.

## Build

### Make

```bash
make
```

### CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/thermal_simd --help
./build/thermal_simd --no-avx512 --interval=100 --down-ratio=1.3 --duration-sec=5
```

If hardware perf access is intentionally unavailable, the software telemetry path can be exercised with:

```bash
TSD_FAKE_PERF=1 ./build/thermal_simd --no-avx512 --duration-sec=1 --metrics-port=0
```

## SIMD dispatch and executable-memory security

The public compatibility API still exposes `tsd_trampoline_patch(width)`, but the function no longer patches executable bytes at runtime. Initialization performs:

1. `mmap(..., PROT_READ | PROT_WRITE)` for a non-executable code table.
2. Construction of the three CET/IBT-compatible slots, each beginning with `ENDBR64`.
3. `mprotect(..., PROT_READ | PROT_EXEC)` for the complete table.
4. `/proc/self/maps` verification that the resulting mapping is readable/executable and not writable.

A runtime width transition then performs only an atomic active-slot selection. Because published code is immutable, there is no double-buffer reclamation race for readers that already loaded a function pointer.

The built-in demonstration modes are:

- SSE4.1: 128-bit XMM integer multiply.
- AVX2: 256-bit YMM broadcast + integer multiply, followed by `vzeroupper`.
- AVX-512: 512-bit ZMM broadcast + integer multiply, followed by `vzeroupper`.

The scalar input/output shim is retained for backward compatibility; the wide variants broadcast the scalar operands across all lanes so the returned low lane has identical semantics.

## Runtime flags

- `--config=FILE` load overrides from a JSON file.
- `--interval=MS` monitoring interval (default 50).
- `--down-count=N` throttle observations before downgrade (default 3).
- `--up-count=N` stable observations before upgrade (default 5).
- `--down-ratio=R` CPI degradation threshold (default 1.5).
- `--cooldown-down=MS` cooldown after downgrade (default 1000).
- `--cooldown-up=MS` cooldown after upgrade (default 2000).
- `--min-dwell=MS` minimum time at a width (default 200).
- `--no-avx512` disable AVX-512 selection.
- `--duration-sec=S` demonstration runtime (default 10).
- `--work-iters=N` inner work iterations per tick (default 10,000,000).
- `--degraded-timeout-sec=S` fail closed after prolonged software telemetry (default 120).
- `--log-level=LEVEL` one of `error`, `warn`, `info`, `debug`.
- `--health-check` validate perf counters, telemetry and immutable trampoline integrity then exit.

### Predictive policy

- `--temp-ceiling=°C` controller ceiling (default 92).
- `--safety-margin=°C` guard band below the ceiling for upgrades (default 4).
- `--emergency-margin=°C` emergency buffer that forces the conservative path (default 10).
- `--predictive-alpha=A` CPI EWMA alpha (default 0.25).
- `--coeff-path=PATH` ARX coefficient bundle (default `config/controller_coeffs.json`).

The current policy is a **model-assisted discrete candidate optimizer**, not a general continuous MPC solver. It forecasts thermal state, scores the available SIMD widths against configured SLOs and transition penalties, and falls back to hysteresis when predictive inputs are stale or invalid.

### Telemetry fusion

- `--telemetry-interval=MS` collector interval (default 50).
- `--telemetry-max-skew=MS` allowable skew between collectors (default 150).
- `--telemetry-ewma=A` telemetry CPI EWMA alpha (default 0.25).
- `--telemetry-profile=PATH` optional telemetry profile manifest.

### Metrics and observability

- `--metrics-port=PORT` Prometheus endpoint port (default 9464, `0` disables).
- `--metrics-bind=ADDR` bind address (default `127.0.0.1`).
- `--metrics-cert=PATH` / `--metrics-key=PATH` enable TLS.
- `--metrics-ca=PATH` optional client CA bundle.
- `--metrics-require-client-auth` enable mutual TLS.
- `--metrics-basic-auth=user:pass` enable HTTP basic authentication.
- `--statsd-host=HOST` enable StatsD output.
- `--statsd-port=PORT` StatsD UDP port (default 8125).

Tracked signals include perf fallback/degraded events, telemetry sensor drop/recovery, width transitions/failures, health-check failures, predictive forecast errors and metrics flush timing.

## Health check

```bash
./build/thermal_simd --health-check
```

The health check validates hardware telemetry availability where required and calls the trampoline self-validator. The validator confirms the active payload, CET/IBT landing pad and actual RX-only page permissions.

## Validation

See [`docs/testing-matrix.md`](docs/testing-matrix.md) for subsystem coverage.

Public GitHub Actions now include:

- `.github/workflows/ci.yml` — normal configure/build/CTest pipeline.
- `.github/workflows/security.yml` — immutable executable-memory, attestation and health-check regressions.
- `.github/workflows/sandbox.yml` — telemetry/policy tests plus a software-perf degraded-mode runtime exercise.

Hardware-in-the-loop validation remains separate because GitHub-hosted runners cannot guarantee AVX-512, MSR access, deterministic perf permissions or repeatable thermal behavior:

```bash
ci/hw-smoke.sh
ci/thermal-soak.sh
```

The HIL pipeline in `ci/pipeline.yml` targets runners tagged `hil` and `avx512` for hardware smoke, stress and thermal-soak stages.

## Packaging

- `packaging/Dockerfile` builds the runtime container.
- `packaging/systemd/thermal-simd.service` provides a hardened systemd deployment example.
- `packaging/kubernetes/daemonset.yaml` demonstrates the required host telemetry mounts/capabilities.

Containers still need the relevant perf/MSR permissions for hardware telemetry. Do not grant broader capabilities than the deployment actually requires.

## Security notes

- Production dispatch does not use PKU as a substitute for W^X.
- Production code pages are never simultaneously writable and executable.
- Runtime SIMD transitions do not rewrite code pages.
- The active trampoline hash is recomputed when a slot is selected and can be checked through the attestation API.
- `ENDBR64` supports x86 CET Indirect Branch Tracking; it is unrelated to CET shadow-stack alignment requirements.

## License

This project is distributed under a proprietary commercial license. See [LICENSE](LICENSE) for full terms.
