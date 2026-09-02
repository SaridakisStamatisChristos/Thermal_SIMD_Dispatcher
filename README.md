# Thermal-Aware SIMD Dispatcher

Linux x86-64 runtime and embeddable library that adaptively selects SSE4.1, AVX2, or AVX-512 application kernels from live performance-counter and thermal telemetry.

The project is built around a strict safety boundary: executable code is created once on a writable/non-executable page, the complete table is sealed read+execute, and runtime transitions only select immutable implementations. The controller can fail closed to SSE4.1 without rewriting executable memory.

## Highlights

- immutable W^X executable table with CET/IBT `ENDBR64` landing pads;
- genuine 128-bit, 256-bit, and 512-bit built-in width-exercising payloads;
- application-facing registered-kernel API for real SSE4.1/AVX2/AVX-512 workloads;
- cooperative chunked dispatch that re-resolves width between bounded work chunks;
- process-wide embeddable adaptive runtime lifecycle;
- time-scaled `perf_event_open` cycle/instruction groups with LLC enrichment;
- fail-closed software/degraded mode with bounded hardware re-probing;
- raw package-temperature safety channel separated from EWMA-filtered control telemetry;
- cpuset-aware workload/monitor placement and owner-affinity restoration;
- package-aware thermal-zone/`hwmon` discovery and optional APERF/MPERF/MSR telemetry;
- predictive discrete width scoring with conservative hysteresis fallback;
- TLS/basic-auth Prometheus exporter with separate `/healthz` and `/readyz` semantics;
- GCC, Clang, ASan, UBSan, focused TSan, Make, staged install, external consumer, Docker, security, and sandbox validation.

Hosted CI validates software invariants and deterministic fault handling. Claims about sustained AVX-512 thermal/power behavior require the manual HIL workflow on compatible hardware; the repository does not treat a runner label as hardware evidence.

## Platform

- Linux 5.9+ on x86-64.
- SSE4.1 minimum ISA.
- `CAP_PERFMON` or equivalent `perf_event_open` permission for hardware-counter mode.
- OpenSSL development/runtime libraries for metrics TLS and trampoline attestation hashing.
- Optional `/dev/cpu/*/msr` read access for MSR-backed APERF/MPERF telemetry.

The runtime reads the caller's allowed affinity mask instead of assuming CPU 0. Hardware perf groups remain attached to the original workload TID across hot recovery; the owner affinity is restored on cleanup. When the cpuset permits it, the monitor uses a distinct allowed CPU.

## Build

### Make

```bash
make
./build-make/thermal_simd --help
```

The compatibility Makefile builds the same production source set as CMake, keeps all artifacts under `build-make/`, and links the mixed C/C++ runtime closure through the C++ final linker.

### CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Install / consume

Version **0.4.0** installs the executable, public headers, static core library, versioned CMake package, and controller coefficient bundle.

```bash
cmake -S . -B build-install -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-install -j
sudo cmake --install build-install
```

External CMake projects can consume the installed package with:

```cmake
find_package(thermal_simd_dispatcher 0.3 CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE thermal::simd)
```

Quality Gates build and execute a separate pure-C consumer against the staged installation, so package usability is validated independently of the source tree.

## Embeddable runtime lifecycle

`runtime.h` owns the process-wide adaptive control lifecycle. The current architecture intentionally supports one active runtime/control domain per process.

```c
#include <thermal/simd/runtime.h>

static void one_unit_of_work(void) {
    /* Optional baseline/probe workload. */
}

int run_runtime(void) {
    tsd_runtime_t *runtime = NULL;
    if (tsd_runtime_start(&runtime, one_unit_of_work) != 0) {
        return -1;
    }

    /* Application work can now use adaptive dispatch objects. */

    tsd_runtime_request_stop(runtime);
    if (tsd_runtime_stop(runtime) != 0) {
        return -1;
    }

    /* stop() leaves an inert tombstone so a stale stopped handle cannot alias
       or stop a later runtime generation. Destroy only after no thread can
       reference the stopped handle. */
    return tsd_runtime_destroy(runtime);
}
```

Lifecycle guarantees:

- startup always begins at SSE4.1;
- wider widths are authorized only after live perf/thermal policy allows them;
- a second simultaneous runtime returns `EBUSY`;
- `tsd_runtime_is_running()` and `tsd_runtime_perf_mode()` are synchronized against stop;
- `tsd_runtime_request_stop()` on a stale stopped handle is a no-op and cannot affect a later runtime generation;
- `tsd_runtime_stop()` releases monitor/perf/telemetry resources but keeps an inert public handle;
- `tsd_runtime_destroy()` releases that stopped handle and returns `EBUSY` if called on the active runtime.

Using a handle after `tsd_runtime_destroy()` is an ordinary freed-object programming error.

## Application-facing adaptive dispatch

Applications register ordinary kernel variants rather than using the built-in scalar demonstration shim:

```c
#include <thermal/simd/adaptive_dispatch.h>

static void process_sse41(void *ctx, size_t n) { /* SSE4.1 */ }
static void process_avx2(void *ctx, size_t n)   { /* AVX2 */ }
static void process_avx512(void *ctx, size_t n) { /* AVX-512 */ }

void process(void *ctx, size_t n) {
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

    simd_width_t last = SIMD_SSE41;
    /* Re-check authorization every 4096 work items. */
    (void)tsd_kernel_dispatch_execute_chunked(dispatch, n, 4096, &last);
    tsd_kernel_dispatch_destroy(dispatch);
}
```

The variant table is copied at creation and remains immutable for the object's lifetime. Each dispatch clamps the live selected width to host ISA support and falls back conservatively to SSE4.1 when a wider registered variant is unavailable. Dispatched work contributes to software-perf accounting.

The runtime remains process-global and primarily observes one workload-owner TID. Multi-package/per-thread control domains are intentionally outside the current scope.

For new integrations, the v2 dispatch API adds explicit `(offset, count)` ranges
and status-returning callbacks. Failed callbacks are propagated and are not
counted as completed software work. The original callback API remains source
and ABI compatible. Dispatch objects must not be destroyed while calls are in
flight, and callers retain ownership and synchronization responsibility for the
context pointer.

## Software overhead benchmark

A deterministic direct-call versus dispatch-call microbenchmark is available
without requiring thermal hardware:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DTSD_BUILD_BENCHMARKS=ON
cmake --build build-bench --target benchmark_dispatch -j
./build-bench/benchmark_dispatch 10000000
```

See [`docs/benchmarking.md`](docs/benchmarking.md) for interpretation and
reporting requirements. It intentionally makes no thermal or energy claim.

## Run

```bash
./build/thermal_simd --help
./build/thermal_simd --no-avx512 --interval=100 --duration-sec=5
./build/thermal_simd --run-forever --interval=50
```

`--duration-sec` is wall-clock time. `--work-iters` is an inner workload batch size, not a duration. `--run-forever` runs until SIGINT/SIGTERM; the standalone executable shuts the monitor down cooperatively and restores runtime resources.

To deliberately exercise software perf mode during development:

```bash
TSD_FAKE_PERF=1 ./build/thermal_simd --no-avx512 --duration-sec=1 --metrics-port=0
```

`TSD_FAKE_PERF` is a test/development override. Normal runtime perf loss enters recoverable software mode and periodically re-probes real hardware counters.

## Runtime flags

Core controls include:

- `--config=FILE`
- `--interval=MS`
- `--down-count=N`
- `--up-count=N`
- `--down-ratio=R`
- `--cooldown-down=MS`
- `--cooldown-up=MS`
- `--min-dwell=MS`
- `--no-avx512` / `--allow-avx512`
- `--duration-sec=S`
- `--run-forever`
- `--work-iters=N`
- `--degraded-timeout-sec=S`
- `--health-check`
- `--log-level=error|warn|info|debug`

Predictive controls include `--temp-ceiling`, `--safety-margin`, `--emergency-margin`, `--predictive-alpha`, and `--coeff-path`.

The shipped ARX coefficients are deterministic conservative/default coefficients, not a claim of universal CPU calibration. See [`docs/model-provenance.md`](docs/model-provenance.md).

## Immutable SIMD selection

Initialization performs:

1. `mmap(..., PROT_READ | PROT_WRITE)` on a non-executable page;
2. construction of SSE4.1/AVX2/AVX-512 CET-compatible slots;
3. `mprotect(..., PROT_READ | PROT_EXEC)` for the complete table;
4. `/proc/self/maps` verification that the mapping is RX and not writable.

Runtime transitions publish only an immutable slot pointer. There is no normal runtime RWX window and no code rewrite/reclamation race.

The compatibility API `tsd_trampoline_patch(width)` is now safety checked in production:

- host ISA and configured AVX-512 policy are always enforced;
- while the adaptive runtime is active, wider selections also require live transition permission, fresh/healthy perf state (or explicit software-upgrade opt-in), and current raw-temperature authorization;
- SSE4.1 remains the unconditional fail-closed width.

White-box test builds retain controlled override hooks for deterministic executable-memory fault testing.

## Perf recovery

The primary cycle/instruction group is a live dependency rather than a startup-only assumption. A failed/invalid/non-running group:

1. closes invalid descriptors;
2. enters software/degraded mode;
3. drives the dispatcher to SSE4.1 by default;
4. publishes unhealthy readiness state;
5. schedules bounded hardware re-probing.

Hardware recovery is transactional: open -> RESET/ENABLE -> observe real enabled/running/cycle/instruction progress -> establish a fresh baseline -> commit hardware mode. Initial hardware startup is not counted as a recovery. The optional LLC event has its own recovery schedule.

## Thermal telemetry

Telemetry uses two intentionally distinct channels:

- **raw safety temperature** drives emergency decisions and wider-SIMD authorization;
- **filtered control temperature** is the EWMA channel used for forecasting/control and reporting trends.

A sudden thermal spike therefore cannot be hidden behind EWMA lag. `--telemetry-ewma=0` means filter bypass, never a frozen sensor value.

The bridge tracks raw freshness with `steady_clock`, scores package-relevant thermal-zone/`hwmon` sources (`coretemp`, `k10temp`, `zenpower`, etc.), and normalizes frequency ratio in milli-units (`1000 == 1.0x`).

## Metrics and health

The exporter supports bounded worker concurrency, queue limits, request-size limits, client deadlines, TLS, optional mTLS, Basic Auth, Prometheus, and StatsD.

- `/healthz` = process/exporter liveness. Recoverable dependency degradation does not make the process dead.
- `/readyz` = operational readiness. It requires fresh controller/fusion/perf state and healthy hardware perf counters.

Health JSON exposes controller, fusion, and perf state plus separate:

- `rawTempAvailable` / `rawPackageTempC`
- `filteredTempAvailable` / `filteredPackageTempC`

The legacy `packageTempC` remains a filtered-control alias for compatibility.

Prometheus exposes `tsd_package_temperature_c` and `tsd_package_temperature_available` with `channel="raw_safety"` and `channel="filtered_control"` labels.

## Validation

Hosted GitHub Actions cover:

- Release build + full CTest;
- immutable executable-memory/security regressions;
- software-perf degraded-mode/sandbox regressions;
- GCC Debug and Clang Debug;
- Clang ASan + UBSan;
- focused Clang TSan including telemetry fusion, observability, adaptive dispatch, and runtime lifecycle/stop diagnostics;
- compatibility Make build;
- staged CMake install;
- external pure-C `find_package` consumer;
- Docker build.

The runtime lifecycle regression explicitly exercises concurrent diagnostics during stop and verifies that an old stopped handle cannot terminate a later runtime generation.

See [`docs/testing-matrix.md`](docs/testing-matrix.md) for the full subsystem matrix.

## Hardware-in-the-loop evidence

`.github/workflows/hil.yml` is manual and runs only on self-hosted runners labelled `hil` and `avx512`.

The characterization harness:

- checks AVX-512F and explicitly opts into AVX-512;
- records commit, kernel, CPU model, microcode, affinity, topology, governor, perf settings, and sensor/powercap metadata;
- runs the persistent runtime;
- samples width, liveness/readiness, perf health, frequency, RAPL power where available, **raw safety temperature**, and **filtered control temperature**;
- requires AVX-512 to actually be observed;
- terminates the runtime with SIGTERM and requires clean status-0 shutdown;
- uploads CSV, JSONL, JSON summary, metadata, logs, and final Prometheus output.

The summary keeps the historical `max_temperature_c`/`mean_temperature_c` keys as aliases of the raw safety channel and additionally records explicit raw and filtered statistics.

This repository does **not** claim HIL evidence exists until the workflow has actually run on compatible hardware.

```bash
ci/hw-smoke.sh
SOAK_MINUTES=30 ci/thermal-soak.sh
```

## Packaging

- `packaging/Dockerfile` packages the runtime and coefficient bundle.
- `packaging/systemd/thermal-simd.service` runs persistent mode with `CAP_PERFMON`, graceful SIGTERM, and hardened service restrictions.
- `packaging/kubernetes/daemonset.yaml` includes its ServiceAccount, non-privileged/read-only security context, `CAP_PERFMON`, persistent mode, `/readyz`, and `/healthz` probes.

Default deployment examples intentionally do **not** mount `/dev/cpu` or grant `CAP_SYS_RAWIO`. If a host explicitly requires MSR telemetry, grant only the necessary device access/host-specific permission rather than broadening every deployment with `CAP_SYS_ADMIN`.

## Security notes

- Production dispatch never uses PKU as a substitute for W^X.
- Production code pages are never simultaneously writable and executable.
- Runtime SIMD transitions do not rewrite code pages.
- CET/IBT targets begin with `ENDBR64`.
- Attestation access is synchronized with width selection.
- Failed final RX verification tears down partial initialization.
- SIGSEGV/SIGBUS crash diagnostics use bounded async-signal-safe `write(2)` output before `_exit`.
- Wider compatibility-selector requests cannot bypass an active runtime's live perf/thermal authorization.

## License

Copyright (c) 2025-2026 Stamatis-Christos Saridakis. All rights reserved.

Except for the identified third-party component, this project is
**source-available, not open source**. The published code may be evaluated for
up to 32 consecutive calendar days under the
[PolyForm Free Trial License 1.0.0](LICENSE). Any production use,
non-evaluation commercial use, use after the evaluation period, redistribution,
sublicensing, and broader rights require a separate written commercial
agreement with the copyright holder. See [NOTICE](NOTICE) for the ownership
notice, [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party terms,
and [CONTRIBUTING.md](CONTRIBUTING.md) before proposing changes.
