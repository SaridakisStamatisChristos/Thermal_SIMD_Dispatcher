# Thermal-Aware SIMD Dispatcher

Linux x86-64 research/runtime system that selects between SSE4.1, AVX2 and AVX-512 implementations using performance-counter and thermal telemetry.

The dispatcher uses an **immutable W^X code table**: each implementation is written once while the backing page is `RW` and non-executable, the page is sealed `RX`, and runtime transitions only atomically select an already-sealed slot. No executable page is rewritten during normal operation. The built-in demonstration payloads execute genuine 128-bit, 256-bit and 512-bit vector operations while preserving the legacy scalar shim ABI.

Thermal adaptation combines time-scaled CPI from `perf_event_open`, LLC-miss information, temperature/frequency telemetry, hysteresis, cooldown and minimum dwell. A model-assisted predictive policy can score candidate widths from recent telemetry and an ARX temperature forecast; if the predictive path is unavailable, the runtime falls back to the conservative hysteresis controller.

This repository is intentionally explicit about validation boundaries: hosted CI validates software behavior, memory-permission invariants and degraded-mode logic; AVX-512/MSR/perf/thermal behavior requires a compatible self-hosted HIL machine.

## Platform and prerequisites

- Linux 5.9+ on x86-64.
- SSE4.1 is the minimum supported ISA.
- `CAP_PERFMON` or equivalent `perf_event_open` permission for hardware-counter mode.
- Optional `/dev/cpu/*/msr` access for MSR-backed APERF/MPERF telemetry.
- OpenSSL development/runtime libraries for metrics TLS and trampoline attestation hashing.
- Optional metrics TLS materials when exposing `/metrics` off-host.

The runtime degrades safely when hardware counters or thermal signals are unavailable. By default software-only mode does not upgrade above SSE4.1 unless explicitly allowed.

## Build

### Make

The compatibility Makefile builds the same runtime source set as the CMake core and links OpenSSL through `pkg-config` when available:

```bash
make
```

### CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Install

```bash
cmake -S . -B build-install -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-install -j
sudo cmake --install build-install
```

The CMake install includes the `thermal_simd` executable, public headers, the static core library, CMake export metadata and `controller_coeffs.json` under the configured system configuration directory.

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

A runtime width transition then performs only an active-slot selection. Because published code is immutable, there is no double-buffer reclamation race for readers that already loaded a function pointer.

The built-in demonstration modes are:

- SSE4.1: 128-bit XMM integer multiply.
- AVX2: 256-bit YMM broadcast + integer multiply, followed by `vzeroupper`.
- AVX-512: 512-bit ZMM broadcast + integer multiply, followed by `vzeroupper`.

The scalar input/output shim is retained for backward compatibility; the wide variants broadcast the scalar operands across all lanes so the returned low lane has identical semantics. These payloads are deliberately small width-exercising demonstrations, not claims of application-level AVX2/AVX-512 throughput scaling.

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
- `--coeff-path=PATH` ARX coefficient bundle override.

The build-tree default coefficient bundle is `config/controller_coeffs.json`. Installed/package deployments may use the installed system configuration path, and `TSD_PREDICTIVE_COEFF_PATH` takes precedence when set.

The current policy is a **model-assisted discrete candidate optimizer**, not a general continuous MPC solver. It forecasts thermal state, scores the available SIMD widths against configured SLOs and transition penalties, and falls back to hysteresis when predictive inputs are stale or invalid. Missing temperature is treated as unavailable data rather than as a fictitious `0 C` measurement.

### Telemetry fusion

The production thermal path is documented in [`docs/telemetry-fusion.md`](docs/telemetry-fusion.md).

The direct Linux helper provides temperature and APERF/MPERF or cpufreq ratio data with exponential recovery/backoff. The C++ fusion layer provides freshness/quality arbitration and a collector API. The production bridge seeds the fusion bus from the real direct helper and returns failure for empty fused snapshots, so the fusion layer cannot silently replace hardware telemetry with zeros.

Frequency ratio is normalized in milli-units (`1000 == 1.0x`) across the production bridge and collector API.

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

GitHub Actions include:

- `.github/workflows/ci.yml` — normal release configure/build/full CTest pipeline.
- `.github/workflows/security.yml` — immutable executable-memory, attestation and health-check regressions.
- `.github/workflows/sandbox.yml` — telemetry/policy tests plus a software-perf degraded-mode runtime exercise.
- `.github/workflows/quality.yml` — GCC/Clang debug matrix, Clang ASan+UBSan, Makefile parity, staged CMake install and container build.
- `.github/workflows/hil.yml` — manually triggered hardware validation for self-hosted runners labelled `hil` and `avx512`.

The standard CTest suite also contains bounded smoke registrations for patch-selection stress, signal stress and telemetry fault recovery. Longer HIL runs use:

```bash
ci/hw-smoke.sh
ci/thermal-soak.sh
```

`ci/pipeline.yml` is retained as a GitLab-compatible HIL definition; GitHub users should use `.github/workflows/hil.yml`.

## Packaging

- `packaging/Dockerfile` installs OpenSSL build/runtime dependencies, packages controller coefficients, and keeps `/usr/local/bin/thermal_simd` as the entrypoint so orchestrator arguments replace the default health-check command correctly.
- `packaging/systemd/thermal-simd.service` provides a capability-bounded, hardened deployment example.
- `packaging/kubernetes/daemonset.yaml` uses a non-privileged security context with explicit `PERFMON`/`SYS_ADMIN` capabilities and read-only host telemetry mounts.

Containers still need the permissions actually required by the host's perf/MSR configuration. Prefer narrower host permissions or device ownership over adding broader capabilities when the deployment environment allows it.

## Security notes

- Production dispatch does not use PKU as a substitute for W^X.
- Production code pages are never simultaneously writable and executable.
- Runtime SIMD transitions do not rewrite code pages.
- The active trampoline hash is recomputed when a slot is selected and can be checked through the attestation API.
- `ENDBR64` supports x86 CET Indirect Branch Tracking; it is unrelated to CET shadow-stack alignment requirements.
- Crash diagnostics avoid formatted libc I/O in SIGSEGV/SIGBUS handlers and emit only bounded data through `write(2)` before `_exit`.

## License

This project is distributed under a proprietary commercial license. See [LICENSE](LICENSE) for full terms.
