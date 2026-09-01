# Validation Matrix

This document maps dispatcher subsystems to automated coverage and hardware-only validation. Public CI is intentionally separated from hardware-in-the-loop checks because hosted runners cannot guarantee AVX-512 availability, MSR access, stable perf permissions or repeatable thermal behavior.

## Unit tests

| Subsystem | Test binary | Path | Coverage |
| --- | --- | --- | --- |
| Dispatcher core | `test_thermal_simd` | [`tests/test_thermal_simd.c`](../tests/test_thermal_simd.c) | Width transitions, fallback paths, policy timing and fault escalation. |
| Immutable executable-memory dispatch | `test_trampoline_security` | [`tests/patcher/test_trampoline_security.cpp`](../tests/patcher/test_trampoline_security.cpp) | RX-only mappings, CET/IBT landing pads, native 128/256/512-bit payload encodings, fail-closed selection and attestation mismatch detection. |
| Predictive policy | `test_policy_controller` | [`tests/policy/test_policy_controller.c`](../tests/policy/test_policy_controller.c) | Dwell timers, emergency fallback, stale telemetry and coefficient reload behavior. |
| ARX estimator | `test_arx_model` | [`tests/policy/test_arx_model.cpp`](../tests/policy/test_arx_model.cpp) | Coefficient parsing, temperature forecasting and residual handling. |
| Telemetry fusion | `test_telemetry`, `test_telemetry_fusion`, `test_telemetry_fusion_stress` | [`tests/telemetry/`](../tests/telemetry) | Sensor normalization, staleness, fusion-thread behavior and concurrent access. |
| Config parsing | `test_config_parser`, `test_runtime_config_cli` | [`tests/`](../tests) | CLI/env precedence and malformed override rejection. |
| Statistics helpers | `test_statistics` | [`tests/test_statistics.c`](../tests/test_statistics.c) | EWMA/trimmed statistics used by policy heuristics. |
| Observability | `test_logging_metrics`, `test_observability_metrics` | [`tests/observability/`](../tests/observability) | Counters, exporters, TLS/auth configuration and snapshots. |

All registered unit tests run through `ctest` when `BUILD_TESTING=ON`.

## Executable-memory security invariant

Production trampolines are no longer rewritten at runtime. Initialization creates the complete code table as `RW` and non-executable, copies the canonical SSE4.1/AVX2/AVX-512 payloads, then changes the mapping to `RX` before publishing any slot. Runtime transitions atomically select an immutable slot.

`test_trampoline_security` verifies:

1. every active indirect-call target begins with `ENDBR64`;
2. the active mapping is reported by `/proc/self/maps` as readable/executable and not writable;
3. fault-injected transitions do not change the selected width;
4. no compatibility "inactive" slot is reported writable;
5. AVX2 contains a YMM broadcast and AVX-512 contains a ZMM broadcast, preventing regression to XMM-only encodings;
6. attestation rejects a deliberately modified test-only immutable payload.

The test-only override path allocates a fresh RW page, writes the injected payload, seals it RX and never rewrites it. Production builds do not expose this override path.

## Public GitHub Actions

| Workflow | Trigger | Purpose |
| --- | --- | --- |
| [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) | push to `main`, pull request | Standard configure/build/CTest regression suite. |
| [`.github/workflows/security.yml`](../.github/workflows/security.yml) | push to `main`, pull request, weekly | Focused immutable-trampoline, attestation and health-check security regressions. |
| [`.github/workflows/sandbox.yml`](../.github/workflows/sandbox.yml) | push to `main`, pull request, weekly | Policy/telemetry tests plus a forced software-perf degraded-mode runtime exercise. |

These workflows require no private signing material and are expected to be runnable on ordinary GitHub-hosted Linux runners.

## Integration and smoke

| Scenario | Script | Notes |
| --- | --- | --- |
| Build + basic runtime smoke | [`tests/compile.sh`](../tests/compile.sh), [`tests/smoke.sh`](../tests/smoke.sh) | Useful for local/pre-merge validation. |
| Hardware capability/health gate | [`ci/hw-smoke.sh`](../ci/hw-smoke.sh) | Requires real perf/MSR permissions and the intended CPU feature set. |

## Stress and fault injection

| Harness | Binary | Description |
| --- | --- | --- |
| Transition churn | `stress_patch_request` | Repeated immutable width selections with injected failures. The compatibility name remains "patch" although production code pages are not rewritten. |
| Signal storm | `stress_signal_storm` | Exercises signal handling while width selections occur. |
| Telemetry faults | `stress_telemetry_faults` | Feeds malformed/dropout snapshots and checks safe fallback behavior. |

These targets live in [`tests/stress/`](../tests/stress) and are intended primarily for controlled HIL runners where the advertised ISA is guaranteed.

## Hardware-in-the-loop

| Stage | Definition | Purpose |
| --- | --- | --- |
| `hardware-smoke` | [`ci/pipeline.yml`](../ci/pipeline.yml) | Rebuild and run `ci/hw-smoke.sh` on bare metal. |
| `stress-suite` | [`ci/pipeline.yml`](../ci/pipeline.yml) | Run transition/signal/telemetry stress harnesses on known hardware. |
| `thermal-soak` | [`ci/pipeline.yml`](../ci/pipeline.yml) | Long-running thermal regression and policy-stability check. |

Provisioning guidance is in [`docs/ci-hil.md`](ci-hil.md). HIL runners should advertise `hil` and `avx512` only when those capabilities are actually present.

## Release review requirements

A release candidate should not be promoted solely because public CI is green. Reviewers should also confirm:

- the latest public `CI`, `Security Regression` and `Sandbox Regression` workflows passed;
- at least one representative hardware-smoke run passed on the deployment CPU family;
- AVX-512 deployments have a successful AVX-512 HIL run rather than relying on CPUID mocks;
- thermal-soak results show no oscillatory width-selection behavior under sustained load;
- the active trampoline self-validator reports RX-only mappings on the deployment kernel;
- metrics/health endpoints are bound and authenticated according to the deployment threat model.

## Manual runbooks

- [`docs/runbooks/sensor-failure.md`](runbooks/sensor-failure.md) — telemetry dropout/degraded-mode remediation.
- [`docs/runbooks/policy-divergence.md`](runbooks/policy-divergence.md) — forecast drift and controller triage.
- [`docs/runbooks/patcher-attestation-alert.md`](runbooks/patcher-attestation-alert.md) — active-payload attestation mismatch handling.
- [`docs/security/threat-model.md`](security/threat-model.md) — deployment threat model and remaining operator controls.

## Remaining validation expansion

- Add compiler/sanitizer matrix coverage (GCC + Clang, ASan/UBSan; TSan where executable-memory test behavior is supported).
- Record HIL CPU model/microcode/kernel metadata with thermal-soak artifacts.
- Add calibrated throughput-per-watt measurements for representative real kernels, not only the built-in demonstration payload.
- Add a long-haul degraded-mode recovery test when hardware-counter hot re-probing is implemented.
