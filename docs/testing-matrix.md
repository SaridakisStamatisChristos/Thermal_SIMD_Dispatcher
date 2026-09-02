# Validation Matrix

This document maps dispatcher subsystems to automated coverage and hardware-only validation. Hosted CI is intentionally separated from hardware-in-the-loop checks because GitHub-hosted runners cannot guarantee AVX-512 availability, MSR access, stable perf permissions or repeatable thermal behavior.

## Unit and bounded integration tests

| Subsystem | Test binary | Path | Coverage |
| --- | --- | --- | --- |
| Dispatcher core | `test_thermal_simd` | [`tests/test_thermal_simd.c`](../tests/test_thermal_simd.c) | Width transitions, fallback paths, policy timing, explicit thermal authorization for upgrades and fault escalation. |
| Perf resilience | `test_perf_resilience` | [`tests/perf/test_perf_resilience.c`](../tests/perf/test_perf_resilience.c) | Runtime group-loss fail-closed behavior, continuous software-mode upgrade authorization, allowed-cpuset CPU selection and CPU-coherent fusion reference counting. |
| Immutable executable-memory dispatch | `test_trampoline_security` | [`tests/patcher/test_trampoline_security.cpp`](../tests/patcher/test_trampoline_security.cpp) | RX-only mappings, CET/IBT landing pads, native 128/256/512-bit payload encodings, fail-closed selection and attestation mismatch detection. |
| Predictive policy | `test_policy_controller` | [`tests/policy/test_policy_controller.c`](../tests/policy/test_policy_controller.c) | Candidate convergence/stability, explicit fallback, missing-temperature semantics and guarded upgrade behavior. |
| ARX estimator | `test_arx_model` | [`tests/policy/test_arx_model.cpp`](../tests/policy/test_arx_model.cpp) | Coefficient parsing, temperature forecasting, residual handling and explicit coefficient reload. |
| Telemetry fusion | `test_telemetry`, `test_telemetry_fusion`, `test_telemetry_fusion_stress` | [`tests/telemetry/`](../tests/telemetry) | Sensor normalization, frequency-ratio units, bridge publication, standalone bridge defaults, staleness, fusion-thread behavior and concurrent access. |
| Config parsing | `test_config_parser`, `test_runtime_config_cli` | [`tests/`](../tests) | CLI/env precedence and malformed override rejection. Production fusion consumes configured interval/freshness/EWMA values; unsupported profile manifests are explicitly rejected by the bridge rather than silently ignored. |
| Statistics helpers | `test_statistics` | [`tests/test_statistics.c`](../tests/test_statistics.c) | EWMA/trimmed statistics used by policy heuristics. |
| Observability | `test_logging_metrics`, `test_observability_metrics` | [`tests/observability/`](../tests/observability) | Counters, exporters, TLS/auth configuration, degraded perf readiness and snapshots. Controller heartbeat is refreshed by the runtime monitor even when no transition is recommended. |

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
| [`.github/workflows/quality.yml`](../.github/workflows/quality.yml) | push to `main`, pull request, manual | GCC/Clang debug builds, Clang ASan+UBSan, Makefile parity, staged install and Docker build. |
| [`.github/workflows/hil.yml`](../.github/workflows/hil.yml) | manual | Bare-metal/self-hosted hardware smoke, stress and soak on runners labelled `hil` + `avx512`. |

The first four workflows are expected to run on ordinary GitHub-hosted Linux runners. The HIL workflow requires an explicitly provisioned self-hosted machine.

## Stress and fault injection

| Harness | CTest smoke | Description |
| --- | --- | --- |
| `stress_patch_request` | `stress_patch_request_smoke` | Repeated immutable width selections with injected failures. The compatibility name remains "patch" although production code pages are not rewritten. |
| `stress_signal_storm` | `stress_signal_storm_smoke` | Exercises signal activity while width selections occur. |
| `stress_telemetry_faults` | `stress_telemetry_faults_smoke` | Exercises telemetry dropout/recovery behavior. |

The CTest registrations use deliberately bounded arguments so they can run in hosted CI. Longer parameters remain available to HIL jobs.

## Hardware-in-the-loop

The canonical GitHub HIL entrypoint is [`.github/workflows/hil.yml`](../.github/workflows/hil.yml). It runs three ordered stages on `[self-hosted, hil, avx512]`:

1. `hardware-smoke` — build plus `ci/hw-smoke.sh`;
2. `stress-suite` — transition, signal and telemetry-fault stress;
3. `thermal-soak` — `ci/thermal-soak.sh` with a caller-selected duration.

[`ci/pipeline.yml`](../ci/pipeline.yml) is retained for GitLab-compatible deployments. Provisioning guidance is in [`docs/ci-hil.md`](ci-hil.md). A runner should advertise `avx512` only when AVX-512 is genuinely executable and the required perf/MSR permissions are present.

## Packaging validation

`quality.yml` validates all three supported build/deployment paths that do not require privileged hardware access:

- the compatibility `Makefile`;
- CMake build plus staged `cmake --install` output;
- `packaging/Dockerfile` image construction.

The staged install gate checks that the runtime binary, controller coefficient bundle and public trampoline header are actually present.

The Kubernetes example deliberately uses `/readyz` for strict adaptive-runtime readiness and the responsive `/metrics` endpoint for liveness. This prevents a recoverable perf-counter outage from causing a restart loop while the in-process hot-reprobe path is active.

## Release review requirements

A release candidate should not be promoted solely because hosted CI is green. Reviewers should also confirm:

- `CI`, `Security Regression`, `Sandbox Regression` and `Quality Gates` passed on the release commit/PR;
- at least one representative hardware-smoke run passed on the deployment CPU family;
- AVX-512 deployments have a successful AVX-512 HIL run rather than relying on CPUID mocks;
- soak/stress results show no oscillatory or unsafe width-selection behavior under sustained load;
- runtime perf-event loss enters software/degraded mode, wider SIMD remains blocked unless explicitly opted in, and subsequent hot recovery is observed on the deployment kernel;
- the hardware recovery event is published only after a fresh baseline validates;
- optional LLC loss/recovery behaves independently of the primary cycle/instruction group;
- the active trampoline self-validator reports RX-only mappings on the deployment kernel;
- metrics/health endpoints are bound and authenticated according to the deployment threat model.

## Manual runbooks

- [`docs/runbooks/sensor-failure.md`](runbooks/sensor-failure.md) — telemetry dropout/degraded-mode remediation.
- [`docs/runbooks/policy-divergence.md`](runbooks/policy-divergence.md) — forecast drift and controller triage.
- [`docs/runbooks/patcher-attestation-alert.md`](runbooks/patcher-attestation-alert.md) — active-payload attestation mismatch handling.
- [`docs/security/threat-model.md`](security/threat-model.md) — deployment threat model and remaining operator controls.

## Remaining validation expansion

- Add TSan where signal/executable-memory test behavior is reliable enough to avoid false positives.
- Record HIL CPU model, microcode, kernel and governor metadata as workflow artifacts.
- Add calibrated throughput-per-watt measurements for representative real kernels, not only the built-in demonstration payload.
- Add a long-haul HIL fault-injection run that revokes/restores perf access and records software-to-hardware re-probe timing plus optional LLC recovery timing.
