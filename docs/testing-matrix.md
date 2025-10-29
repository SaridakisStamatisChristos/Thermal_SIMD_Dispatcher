# Validation Matrix

This document maps the dispatcher subsystems to automated coverage and operator checklists so production rollouts can rely on deterministic guardrails instead of tribal knowledge.

## Unit Tests

| Subsystem | Test Binary | Path | Notes |
| --- | --- | --- | --- |
| Dispatcher core (trampoline wiring, downgrade paths) | `test_thermal_simd` | [`tests/test_thermal_simd.c`](../tests/test_thermal_simd.c) | Exercises scalar/SIMD transitions, W^X enforcement, and failure escalation. |
| Predictive controller | `test_policy_controller` | [`tests/policy/test_policy_controller.c`](../tests/policy/test_policy_controller.c) | Validates dwell timers, emergency fallbacks, and coefficient reload behavior. |
| Telemetry fusion | `test_telemetry` | [`tests/telemetry/test_telemetry.cpp`](../tests/telemetry/test_telemetry.cpp) | Mocks perf/MSR inputs to ensure normalization, staleness guards, and degraded flags. |
| Config parsing | `test_config_parser` | [`tests/test_config_parser.c`](../tests/test_config_parser.c) | Confirms CLI/env precedence and rejects malformed telemetry/controller overrides. |
| Statistics helpers | `test_statistics` | [`tests/test_statistics.c`](../tests/test_statistics.c) | Guards percentile and EWMA helpers used by controller heuristics. |

All unit binaries build via `cmake --build build --target test_thermal_simd test_policy_controller test_telemetry ...` and execute under `ctest` when `BUILD_TESTING=ON`.

## Integration & Smoke

| Scenario | Script | Tags |
| --- | --- | --- |
| Build + basic health check | [`tests/compile.sh`](../tests/compile.sh), [`tests/smoke.sh`](../tests/smoke.sh) | Runs on public CI and pre-merge branches. |
| Capability/self-test gate | [`ci/hw-smoke.sh`](../ci/hw-smoke.sh) | Requires AVX-512 + perf access; validates `--health-check`. |

The smoke suite compiles the dispatcher, runs `--health-check`, and captures metrics/log assertions expected in staging.

## Stress & Fault Injection

| Harness | Binary | Description |
| --- | --- | --- |
| Patch churn | `stress_patch_request` | Validates double-buffer trampolines under sustained AVX width flips. |
| Signal storm | `stress_signal_storm` | Ensures signal handling remains re-entrant while patching occurs. |
| Telemetry faults | `stress_telemetry_faults` | Feeds malformed snapshots to verify safe downgrade and alerting. |

Targets live in [`tests/stress/`](../tests/stress) and run automatically in the `stress` stage of `ci/pipeline.yml` on `hil`/`avx512` runners.

## Hardware-in-the-Loop

| Stage | Definition | Purpose |
| --- | --- | --- |
| `hardware-smoke` | [`ci/pipeline.yml`](../ci/pipeline.yml) → `hardware-smoke` | Rebuilds and executes `ci/hw-smoke.sh` on bare metal. |
| `stress-suite` | [`ci/pipeline.yml`](../ci/pipeline.yml) → `stress-suite` | Runs all stress harnesses with production-grade parameters. |
| `thermal-soak` | [`ci/pipeline.yml`](../ci/pipeline.yml) → `thermal-soak` | Long-running thermal regression check; see [`ci/thermal-soak.sh`](../ci/thermal-soak.sh). |

Provisioning and operations guidance live in [`docs/ci-hil.md`](ci-hil.md); the fleet must expose the `hil` and `avx512` tags for the stages above to schedule.

## Security & Sandbox Coverage

| Workflow | Definition | Coverage Focus | Automation Notes | Release Review Requirement |
| --- | --- | --- | --- | --- |
| Supply-chain attestation | [`ci/security.yml`](../ci/security.yml) | Verifies attestation bundle signatures, SBOM drift, and binary provenance against `docs/security/threat-model.md`. | Runs on nightly + release-candidate branches with private signing materials. Fails open when credentials are unavailable. | Release manager must confirm artifacts uploaded and checklist in [`docs/security/threat-model.md`](security/threat-model.md) is satisfied before promoting. |
| Sandbox fuzzing | [`ci/sandbox.yml`](../ci/sandbox.yml) | Exercises `ci/sandbox/run.sh` dropout/spike scenarios to surface telemetry, patching, and metrics regressions. | Nightly on hosts labeled `sandbox`; artifacts archived under `artifacts/YYYYmmdd-HHMMSS/`. | QA lead reviews sandbox artifacts per [`docs/sandbox-workflow.md`](sandbox-workflow.md) exit criteria during release sign-off. |

The workflows above surface defects early, but their results are not yet wired into automated release gates. Treat failures and missing runs as blocking items during release reviews until that integration ships.

## Manual Runbooks

The following runbooks plug the remaining operational gaps and guide the manual checks noted above:

- [`docs/runbooks/sensor-failure.md`](runbooks/sensor-failure.md) covers telemetry dropouts and degraded mode remediation.
- [`docs/runbooks/policy-divergence.md`](runbooks/policy-divergence.md) walks through controller forecast drift triage.
- [`docs/runbooks/patcher-attestation-alert.md`](runbooks/patcher-attestation-alert.md) describes the manual attestation validation performed when `ci/security.yml` cannot complete.
- [`docs/security/threat-model.md`](security/threat-model.md) enumerates attestation requirements validated during release reviews.

## Roadmap Items

- Promote `ci/security.yml` verdicts to a mandatory release gate once signing infrastructure is accessible to CI.
- Expand `ci/sandbox.yml` to cover long-haul fuzzing and feed results into automated regression triage.
