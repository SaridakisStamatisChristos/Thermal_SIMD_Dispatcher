# Thermal-Aware SIMD Dispatcher

Linux x86-64 systems runtime and embeddable library that selects between SSE4.1, AVX2 and AVX-512 implementations using performance-counter and thermal telemetry.

The dispatcher uses an **immutable W^X code table**: each built-in implementation is written once while the backing page is `RW` and non-executable, the page is sealed `RX`, and runtime transitions only atomically select an already-sealed slot. No executable page is rewritten during normal operation. The built-in demonstration payloads execute genuine 128-bit, 256-bit and 512-bit vector operations while preserving the legacy scalar shim ABI.

Thermal adaptation combines time-scaled CPI from `perf_event_open`, LLC-miss information, raw safety telemetry, filtered forecasting telemetry, hysteresis, cooldown and minimum dwell. A model-assisted predictive policy can score candidate widths from recent telemetry and an ARX temperature forecast; if the predictive path is unavailable, the runtime falls back to the conservative hysteresis controller.

Runtime failures are intentionally asymmetric and fail closed. Loss of the primary perf cycle/instruction group enters software/degraded mode and drives the dispatcher to SSE4.1 by default while hardware counters are periodically re-probed. Once telemetry fusion is active, loss of package-temperature telemetry also blocks wider SIMD authorization and immediately selects SSE4.1 if a wider slot is active. Downgrades remain available throughout degraded operation.

This repository is intentionally explicit about validation boundaries: hosted CI validates software behavior, memory-permission invariants, degraded-mode logic, compiler/sanitizer portability, package construction, external consumer integration and deterministic fault injection; AVX-512/MSR/perf/thermal behavior requires a compatible self-hosted HIL machine.

## Platform and prerequisites

- Linux 5.9+ on x86-64.
- SSE4.1 is the minimum supported ISA.
- `CAP_PERFMON` or equivalent `perf_event_open` permission for hardware-counter mode.
- Optional `/dev/cpu/*/msr` access for MSR-backed APERF/MPERF telemetry.
- OpenSSL development/runtime libraries for metrics TLS and trampoline attestation hashing.
- Optional metrics TLS materials when exposing `/metrics` off-host.

The runtime respects the caller's CPU-affinity mask rather than assuming CPU 0 is available, which keeps it compatible with cpusets and container CPU restrictions. A perf context records the original workload TID, pins only that owner thread to a permitted workload CPU, binds initial and recovered perf groups to that same TID, and restores the original owner affinity during normal cleanup. A distinct allowed CPU is selected for the monitor thread when the cpuset permits it.

Hardware-counter loss is recoverable: software mode uses bounded re-probe backoff and rebuilds a fresh hardware baseline before resuming hardware evaluation. Recovery is committed only after RESET/ENABLE succeeds and the group demonstrates real enabled time, running time, cycle progress and instruction progress.

## Build

### Make

The compatibility Makefile builds the same runtime source set as the CMake core, links OpenSSL through `pkg-config` when available, and embeds an absolute build-tree path to the default controller coefficient bundle:

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

Version 0.2 installs the `thermal_simd` executable, public headers, the static core library, a versioned CMake package and `controller_coeffs.json` under the configured system configuration directory. External CMake projects can consume it with:

```cmake
find_package(thermal_simd_dispatcher 0.2 CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE thermal::simd)
```

The Quality Gates workflow builds and runs a separate project from `tests/consumer/` against the staged installation, so installed-package usability is validated independently of the source tree.

## Application-facing adaptive dispatch

The built-in trampoline payload remains useful for validating immutable executable-memory transitions, but applications no longer need to use that scalar demonstration ABI. `adaptive_dispatch.h` exposes ordinary registered application variants:

```c
#include <thermal/simd/adaptive_dispatch.h>

static void process_sse41(void *ctx, size_t n) { /* real SSE4.1 kernel */ }
static void process_avx2(void *ctx, size_t n)   { /* real AVX2 kernel */ }
static void process_avx512(void *ctx, size_t n) { /* real AVX-512 kernel */ }

void run(void *ctx, size_t n) {
    tsd_kernel_variants_t variants = {
        .sse41 = process_sse41,
        .avx2 = process_avx2,
        .avx512 = process_avx512,
        .context = ctx,
    };
    tsd_kernel_dispatch_t *dispatch = NULL;
    if (tsd_kernel_dispatch_create(&variants, &dispatch) != 0) {
        return;
    }

    simd_width_t used = SIMD_SSE41;
    (void)tsd_kernel_dispatch_execute(dispatch, n, &used);
    tsd_kernel_dispatch_destroy(dispatch);
}
```

The variant table is copied when the dispatch object is created and is immutable for that object's lifetime, so concurrent execution does not race registration. Each execution samples the runtime's current width atomically, clamps it to host ISA support, and chooses the widest registered implementation at or below that width. SSE4.1 is mandatory as the conservative fallback. Successfully dispatched work is accounted into the software-perf work counter so degraded-mode adaptation can observe application work rather than only the demo shim.

The adaptive-dispatch API consumes the runtime's current width; it does not implicitly start the standalone monitoring lifecycle. Embedders remain responsible for the runtime/control lifecycle appropriate to their application.

## Run

```bash
./build/thermal_simd --help
./build/thermal_simd --no-avx512 --interval=100 --down-ratio=1.3 --duration-sec=5
```

`--duration-sec` is wall-clock duration. The workload executes batches of `--work-iters` until that monotonic-time deadline is reached, then logs actual elapsed milliseconds, completed iterations and measured iterations/second. The final batch may overshoot the requested deadline slightly by at most one batch.

If hardware perf access is intentionally unavailable, the software telemetry path can be exercised with:

```bash
TSD_FAKE_PERF=1 ./build/thermal_simd --no-avx512 --duration-sec=1 --metrics-port=0
```

`TSD_FAKE_PERF` is a deliberate development/test override and does not hot-reprobe real counters. Normal software fallback caused by runtime perf loss does re-probe hardware with bounded exponential backoff.

## SIMD dispatch and executable-memory security

The public compatibility API still exposes `tsd_trampoline_patch(width)`, but the function no longer patches executable bytes at runtime. Initialization performs:

1. `mmap(..., PROT_READ | PROT_WRITE)` for a non-executable code table.
2. Construction of the three CET/IBT-compatible slots, each beginning with `ENDBR64`.
3. `mprotect(..., PROT_READ | PROT_EXEC)` for the complete table.
4. `/proc/self/maps` verification that the resulting mapping is readable/executable and not writable.

A runtime width transition then performs only an active-slot selection. Because published code is immutable, there is no double-buffer reclamation race for readers that already loaded a function pointer. Failed final RX verification tears down the partial mapping instead of leaving reusable half-initialized state.

The built-in demonstration modes are:

- SSE4.1: 128-bit XMM integer multiply.
- AVX2: 256-bit YMM broadcast + integer multiply, followed by `vzeroupper`.
- AVX-512: 512-bit ZMM broadcast + integer multiply, followed by `vzeroupper`.

The scalar input/output shim is retained for backward compatibility; the wide variants broadcast the scalar operands across all lanes so the returned low lane has identical semantics. These payloads are deliberately small width-exercising demonstrations, not claims of application-level AVX2/AVX-512 throughput scaling. Real application kernels should use the adaptive-dispatch API above.

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
- `--duration-sec=S` wall-clock demonstration runtime (default 10).
- `--work-iters=N` inner workload batch size (default 10,000,000).
- `--degraded-timeout-sec=S` fail closed after prolonged software telemetry (default 120).
- `--log-level=LEVEL` one of `error`, `warn`, `info`, `debug`.
- `--health-check` validate perf counters, telemetry and immutable trampoline integrity then exit.

### Predictive policy

- `--temp-ceiling=°C` controller ceiling (default 92).
- `--safety-margin=°C` guard band below the ceiling for upgrades (default 4).
- `--emergency-margin=°C` emergency buffer that forces the conservative path (default 10).
- `--predictive-alpha=A` CPI EWMA alpha (default 0.25).
- `--coeff-path=PATH` ARX coefficient bundle override.

The build-tree default coefficient bundle is `config/controller_coeffs.json`. Installed/package deployments may use the installed system configuration path, and `TSD_PREDICTIVE_COEFF_PATH` takes precedence when set. The shipped coefficients are a conservative default/demo model rather than a claim of universal CPU calibration; see [`docs/model-provenance.md`](docs/model-provenance.md).

The current policy is a **model-assisted discrete candidate optimizer**, not a general continuous MPC solver. It forecasts thermal state, scores the available SIMD widths against configured SLOs and transition penalties, and falls back to hysteresis when predictive inputs are stale or invalid. Missing temperature is treated as unavailable data rather than as a fictitious `0 C` measurement. Predictive downgrades remain permitted without temperature; predictive upgrades require a valid raw temperature sample, and the central runtime transition gate independently enforces the same no-blind-upgrade rule for the hysteresis path.

### Telemetry fusion

The production thermal path is documented in [`docs/telemetry-fusion.md`](docs/telemetry-fusion.md).

The direct Linux helper provides package-aware temperature and APERF/MPERF or cpufreq ratio data with exponential recovery/backoff. Temperature discovery scores CPU-package-relevant thermal zones and `hwmon` sources such as `coretemp`, `k10temp` and `zenpower` instead of blindly taking the first CPU-like thermal zone.

The C++ fusion layer provides freshness/quality arbitration and a collector API. The production bridge is process-wide and reference-counted, starts on the selected workload CPU, seeds the fusion bus from the real direct helper, and returns failure for empty fused snapshots so the fusion layer cannot silently replace hardware telemetry with zeros. A second process-local consumer requesting another CPU does not silently receive the first CPU's frequency telemetry; it falls back to its own CPU-local direct helper.

Telemetry has two explicit channels:

- **raw safety telemetry** drives emergency decisions, reactive thermal severity and wider-SIMD authorization;
- **EWMA-filtered telemetry** feeds forecasting/control and observability.

A sudden thermal spike therefore cannot be hidden by EWMA lag. `--telemetry-ewma=0` means bypass filtering, not “freeze at the first value.” Frequency ratio is normalized in milli-units (`1000 == 1.0x`) across the production bridge and collector API.

### Perf-event recovery

The cycle/instruction perf group is treated as a live dependency rather than a startup-only assumption. A failed group read, invalid group layout or sustained lack of counter runtime:

1. closes the invalid perf descriptors;
2. enters software/degraded mode;
3. forces the conservative SIMD path by default;
4. publishes unhealthy perf state to readiness;
5. schedules a hardware re-probe.

The primary group is bound to the recorded workload TID both at startup and during hot recovery. Re-probe backoff starts at five seconds and is capped at sixty seconds. A successful reopen must RESET/ENABLE the complete group and demonstrate real enabled/running/cycle/instruction progress while establishing a fresh baseline before hardware mode is committed. Initial hardware startup is not counted as a recovery metric; `perf_recoveries` represents an actual software-to-hardware transition. The optional LLC event has an independent recovery schedule.

### Metrics and observability

- `--metrics-port=PORT` Prometheus endpoint port (default 9464, `0` disables).
- `--metrics-bind=ADDR` bind address (default `127.0.0.1`).
- `--metrics-cert=PATH` / `--metrics-key=PATH` enable TLS.
- `--metrics-ca=PATH` optional client CA bundle.
- `--metrics-require-client-auth` enable mutual TLS.
- `--metrics-basic-auth=user:pass` enable HTTP basic authentication.
- `--statsd-host=HOST` enable StatsD output.
- `--statsd-port=PORT` StatsD UDP port (default 8125).

The HTTP exporter uses bounded worker concurrency, an explicit queue limit, request-size bounds and per-client send/receive deadlines so a slow or incomplete client cannot monopolize liveness/readiness service. Basic-auth comparison is performed without an early-exit secret comparison.

`/healthz` is deliberately a **liveness** endpoint: if the exporter can serve it, the process is live even when recoverable dependencies are degraded. `/readyz` is strict operational readiness and requires fresh controller/fusion state plus healthy hardware perf counters. The JSON response includes explicit controller, fusion and perf state rather than folding perf degradation into the fusion model.

## Health check

```bash
./build/thermal_simd --health-check
```

The health check validates hardware telemetry availability where required and calls the trampoline self-validator. The validator confirms the active payload, CET/IBT landing pad and actual RX-only page permissions. Self-validation and attestation readers are serialized against width selection so an attestation hash cannot race a transition.

## Validation

See [`docs/testing-matrix.md`](docs/testing-matrix.md) for subsystem coverage.

GitHub Actions include:

- `.github/workflows/ci.yml` — normal release configure/build/full CTest pipeline.
- `.github/workflows/security.yml` — immutable executable-memory, attestation and health-check regressions.
- `.github/workflows/sandbox.yml` — telemetry/policy tests plus a software-perf degraded-mode runtime exercise.
- `.github/workflows/quality.yml` — GCC/Clang debug matrix, Clang ASan+UBSan, focused Clang ThreadSanitizer concurrency coverage, Makefile parity, staged install, external `find_package` consumer and container build.
- `.github/workflows/hil.yml` — manually triggered hardware validation for self-hosted runners labelled `hil` and `avx512`.

The standard CTest suite contains bounded smoke registrations for patch-selection stress, signal stress and telemetry fault recovery plus dedicated regressions for runtime perf loss, real counter-progress requirements, cpuset-aware selection/affinity restoration, raw-vs-filtered thermal safety, fusion reference counting, observability availability and application-facing adaptive dispatch.

The HIL workflow is evidence-producing rather than a generic endurance loop. `ci/thermal-soak.sh` and `ci/hil_sampler.py` collect:

- commit, kernel, CPU model, microcode, affinity, topology and governor metadata;
- current/recommended SIMD width over time;
- liveness/readiness and explicit perf-counter mode/health;
- package temperature and frequency telemetry;
- CPU sysfs frequency;
- RAPL package power when the runner exposes powercap energy counters;
- runtime logs and final Prometheus output.

The workflow validates minimum health/perf/temperature coverage and uploads the CSV, JSONL, metadata, logs, summary and metrics as a retained Actions artifact. This repository does **not** claim that HIL evidence exists until that manual workflow has actually run on a compatible machine.

```bash
ci/hw-smoke.sh
SOAK_MINUTES=30 ci/thermal-soak.sh
```

`ci/pipeline.yml` is retained as a GitLab-compatible HIL definition; GitHub users should use `.github/workflows/hil.yml`.

## Packaging

- `packaging/Dockerfile` installs OpenSSL build/runtime dependencies, packages controller coefficients, and keeps `/usr/local/bin/thermal_simd` as the entrypoint so orchestrator arguments replace the default health-check command correctly.
- `packaging/systemd/thermal-simd.service` provides a capability-bounded, hardened deployment example.
- `packaging/kubernetes/daemonset.yaml` is a standalone example containing its ServiceAccount, non-privileged security context, explicit `PERFMON`/`SYS_ADMIN` capabilities, read-only host telemetry mounts, and `/readyz` + `/healthz` probes.

Containers still need the permissions actually required by the host's perf/MSR configuration. Prefer narrower host permissions or device ownership over adding broader capabilities when the deployment environment allows it.

## Security notes

- Production dispatch does not use PKU as a substitute for W^X.
- Production code pages are never simultaneously writable and executable.
- Runtime SIMD transitions do not rewrite code pages.
- The active trampoline hash is recomputed when a slot is selected and can be checked through the attestation API.
- Attestation access is synchronized with width selection; readers cannot observe a partially updated hash.
- `ENDBR64` supports x86 CET Indirect Branch Tracking; it is unrelated to CET shadow-stack alignment requirements.
- Crash diagnostics avoid formatted libc I/O in SIGSEGV/SIGBUS handlers and emit only bounded data through `write(2)` before `_exit`.

## License

This project is distributed under a proprietary commercial license. See [LICENSE](LICENSE) for full terms.
