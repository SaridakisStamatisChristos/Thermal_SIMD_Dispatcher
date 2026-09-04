# Validation Matrix

This document maps dispatcher subsystems to automated coverage and hardware-only validation. Hosted CI is intentionally separated from hardware-in-the-loop checks because GitHub-hosted runners cannot guarantee a particular wide ISA, MSR/RAPL access, stable perf permissions or repeatable thermal behavior.

## Unit and bounded integration tests

| Subsystem | Test binary | Path | Coverage |
| --- | --- | --- | --- |
| Dispatcher core | `test_thermal_simd` | [`tests/test_thermal_simd.c`](../tests/test_thermal_simd.c) | Width transitions, fallback paths, policy timing, explicit thermal authorization for upgrades, fault escalation and EINTR/partial perf-group reads. |
| Application adaptive dispatch | `test_adaptive_dispatch` | [`tests/dispatch/test_adaptive_dispatch.c`](../tests/dispatch/test_adaptive_dispatch.c) | Public registered-kernel API, host/active-width clamping, missing-variant fallback and workload accounting. |
| Perf resilience | `test_perf_resilience` | [`tests/perf/test_perf_resilience.c`](../tests/perf/test_perf_resilience.c) | Runtime group-loss fail-closed behavior, hardware-only upgrade authorization and software fail-closed behavior, real group-progress requirements, allowed-cpuset CPU selection, owner-affinity restoration and CPU-coherent fusion reference counting. |
| Immutable executable-memory dispatch | `test_trampoline_security` | [`tests/patcher/test_trampoline_security.cpp`](../tests/patcher/test_trampoline_security.cpp) | RX-only mappings, CET/IBT landing pads, native 128/256/512-bit payload encodings, fail-closed selection and attestation mismatch detection. |
| Predictive policy | `test_policy_controller` | [`tests/policy/test_policy_controller.c`](../tests/policy/test_policy_controller.c) | Candidate convergence/stability, explicit fallback, missing-temperature semantics and guarded upgrade behavior. |
| ARX estimator | `test_arx_model` | [`tests/policy/test_arx_model.cpp`](../tests/policy/test_arx_model.cpp) | Coefficient parsing, temperature forecasting, residual handling and explicit coefficient reload. |
| Telemetry fusion | `test_telemetry`, `test_telemetry_fusion`, `test_telemetry_fusion_stress` | [`tests/telemetry/`](../tests/telemetry) | Sensor normalization, frequency-ratio units, raw-vs-filtered bridge semantics, raw spike visibility, EWMA bypass, staleness, fusion-thread behavior and concurrent access. |
| Config parsing | `test_config_parser`, `test_runtime_config_cli` | [`tests/`](../tests) | Config-file/CLI precedence, `TSD_LOG_LEVEL`, and malformed override rejection. Production fusion consumes configured interval/freshness/EWMA values; unsupported profile manifests are explicitly rejected rather than silently ignored. |
| Statistics helpers | `test_statistics` | [`tests/test_statistics.c`](../tests/test_statistics.c) | EWMA/trimmed statistics used by policy heuristics. |
| Observability | `test_logging_metrics`, `test_observability_metrics` | [`tests/observability/`](../tests/observability) | Counters, exporters, TLS/auth configuration, strict readiness, liveness separation, slow-client availability and explicit perf state. Controller heartbeat is refreshed by the runtime monitor even when no transition is recommended. |

All registered tests run through `ctest` when `BUILD_TESTING=ON`.

## Executable-memory security invariant

Production trampolines are not rewritten at runtime. Initialization creates the complete code table as `RW` and non-executable, copies the canonical SSE4.1/AVX2/AVX-512 payloads, then changes the mapping to `RX` before publishing any slot. Runtime transitions select an immutable slot.

`test_trampoline_security` verifies:

1. every active indirect-call target begins with `ENDBR64`;
2. the active mapping is reported by `/proc/self/maps` as readable/executable and not writable;
3. fault-injected transitions do not change the selected width;
4. no compatibility "inactive" slot is reported writable;
5. AVX2 contains a YMM broadcast and AVX-512 contains a ZMM broadcast, preventing regression to XMM-only encodings;
6. attestation rejects a deliberately modified test-only immutable payload.

Attestation readers are serialized with width selection so the reported hash cannot race a transition. The test-only override path allocates a fresh RW page, writes the injected payload, seals it RX and never rewrites it. Production builds do not expose this override path.

## GitHub Actions

| Workflow | Trigger | Purpose |
| --- | --- | --- |
| [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) | push to `main`, pull request | Standard release configure/build/full CTest regression suite. |
| [`.github/workflows/security.yml`](../.github/workflows/security.yml) | push to `main`, pull request, weekly | Focused immutable-trampoline, attestation and health-check security regressions. |
| [`.github/workflows/sandbox.yml`](../.github/workflows/sandbox.yml) | push to `main`, pull request, weekly | Policy/telemetry tests plus forced software-perf degraded mode. |
| [`.github/workflows/quality.yml`](../.github/workflows/quality.yml) | push to `main`, pull request, manual | GCC/Clang debug builds, Clang ASan+UBSan, focused Clang TSan, Makefile parity, staged install, external CMake consumer and Docker build. |
| [`.github/workflows/hil.yml`](../.github/workflows/hil.yml) | manual | Bare-metal/self-hosted hardware smoke, stress and evidence-producing characterization on runners labelled `hil` plus the selected `avx2` or `avx512` target. |

The first four workflows are expected to run on ordinary GitHub-hosted Linux runners. The HIL workflow requires an explicitly provisioned self-hosted machine.

### Sanitizer policy

ASan+UBSan runs the broad hosted suite except the signal-storm smoke, where sanitizer signal interposition would obscure the behavior under test. The TSan lane is intentionally narrower: it builds and runs the telemetry-fusion stress, observability exporter and adaptive-dispatch concurrency-relevant targets without mixing ThreadSanitizer with the signal-crash harness.

## Stress and fault injection

| Harness | CTest smoke | Description |
| --- | --- | --- |
| `stress_patch_request` | `stress_patch_request_smoke` | Repeated immutable width selections with injected failures. The compatibility name remains "patch" although production code pages are not rewritten. |
| `stress_signal_storm` | `stress_signal_storm_smoke` | Exercises signal activity while width selections occur. |
| `stress_telemetry_faults` | `stress_telemetry_faults_smoke` | Exercises telemetry dropout/recovery behavior. |

The CTest registrations use deliberately bounded arguments so they can run in hosted CI. Longer parameters remain available to HIL jobs.

## Hardware-in-the-loop

The canonical GitHub HIL entrypoint is [`.github/workflows/hil.yml`](../.github/workflows/hil.yml). It runs three ordered stages on a self-hosted `hil` runner carrying the selected `avx2` or `avx512` label:

1. `hardware-smoke` — build plus `ci/hw-smoke.sh`;
2. `stress-suite` — transition, signal and telemetry-fault stress;
3. `thermal-characterization` — fixed registered-kernel controls followed by an adaptive registered-kernel soak and `ci/hil_sampler.py` for a caller-selected 1–300 minute window.

The characterization stage records and uploads:

- exact commit and UTC start time;
- kernel, CPU model, microcode, CPU-package topology and allowed affinity;
- cpufreq governors and visible thermal/powercap/MSR sources;
- alternating fixed-width registered-kernel trials, checksums, throughput medians and dispersion;
- time-series liveness/readiness;
- current and recommended SIMD width;
- perf mode/counter health and selected workload/monitor CPUs;
- package temperature and frequency ratio;
- CPU sysfs frequency;
- derived package RAPL energy and time-weighted power when top-level powercap energy counters are available;
- raw health JSON snapshots;
- runtime logs and final Prometheus output;
- machine-readable JSON plus human-readable Markdown summaries.

The HIL validator requires at least 95% health/liveness endpoint coverage, 90%
strict readiness, 90% validated hardware-perf coverage, 90% temperature
coverage, repeated checksum-identical fixed controls, and actual application
work at the selected target width. RAPL remains optional because not every
otherwise-valid target exposes package energy through Linux powercap.

[`ci/pipeline.yml`](../ci/pipeline.yml) is retained for GitLab-compatible deployments. Provisioning guidance is in [`docs/ci-hil.md`](ci-hil.md). A runner should advertise an ISA label only when it is genuinely executable and the required perf/thermal permissions are present.

A workflow definition is not evidence by itself. Hardware validation should only be claimed after the manual HIL workflow has actually completed on the relevant CPU family and its artifact has been retained with the release candidate.

## Packaging validation

`quality.yml` validates all supported build/deployment paths that do not require privileged hardware access:

- the compatibility `Makefile`;
- CMake build plus staged `cmake --install` output;
- a separate `tests/consumer` project using `find_package(thermal_simd_dispatcher)` and linking the installed `thermal::simd` target;
- `packaging/Dockerfile` image construction.

The staged install gate checks the runtime binary, coefficient bundle, public adaptive/trampoline headers and versioned CMake package metadata before compiling and executing the external consumer.

The Kubernetes example uses `/readyz` for strict adaptive-runtime readiness and `/healthz` for process liveness. This prevents a recoverable perf-counter outage from causing a restart loop while the in-process hot-reprobe path is active.

## Release review requirements

A release candidate should not be promoted solely because hosted CI is green. Reviewers should also confirm:

- `CI`, `Security Regression`, `Sandbox Regression` and `Quality Gates` passed on the exact release commit/PR head;
- the focused TSan lane is green on that same head;
- the staged external consumer built and ran against the installed package;
- at least one representative hardware-smoke run passed on the deployment CPU family;
- AVX-512 deployments have a successful AVX-512 HIL characterization rather than relying on CPUID mocks;
- HIL artifacts contain CPU/microcode/kernel/governor provenance and usable temperature/perf coverage;
- sustained results show no oscillatory or unsafe width-selection behavior under load;
- runtime perf-event loss enters software/degraded mode, wider SIMD remains blocked unless explicitly opted in, and subsequent hot recovery is observed on the deployment kernel;
- the hardware recovery event is published only after a fresh baseline proves real counter runtime/progress;
- optional LLC loss/recovery behaves independently of the primary cycle/instruction group;
- the active trampoline self-validator reports RX-only mappings on the deployment kernel;
- metrics/health endpoints are bound and authenticated according to the deployment threat model;
- any non-default predictive coefficient bundle has retained calibration provenance as described in [`model-provenance.md`](model-provenance.md).

## Manual runbooks

- [`docs/runbooks/sensor-failure.md`](runbooks/sensor-failure.md) — telemetry dropout/degraded-mode remediation.
- [`docs/runbooks/policy-divergence.md`](runbooks/policy-divergence.md) — forecast drift and controller triage.
- [`docs/runbooks/patcher-attestation-alert.md`](runbooks/patcher-attestation-alert.md) — active-payload attestation mismatch handling.
- [`docs/security/threat-model.md`](security/threat-model.md) — deployment threat model and remaining operator controls.

## Remaining empirical expansion

The codebase now has hosted compiler, memory/UB sanitizer, focused thread sanitizer, installed-consumer, packaging and evidence-producing HIL definitions. The remaining validation boundary is physical rather than a missing hosted-code gate:

- run the manual HIL workflow on representative Intel/AMD deployment families;
- retain long-haul perf-access revoke/restore experiments when the deployment environment can manipulate permissions safely;
- repeat the included controlled registered-kernel procedure with deployment-specific application kernels before making workload-specific claims;
- calibrate and validate platform-specific ARX bundles when predictive accuracy beyond the conservative default/demo model is required.
