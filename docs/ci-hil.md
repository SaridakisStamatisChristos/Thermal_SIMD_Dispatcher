# Hardware-in-the-Loop Continuous Integration

This document describes the bare-metal hardware-in-the-loop (HIL) validation
path, the evidence produced by a run and operator triage steps. Hosted CI checks
software invariants, but it is not treated as evidence for thermal, power or
ISA-specific performance claims.

## GitHub Actions entrypoint

The canonical workflow is [`.github/workflows/hil.yml`](../.github/workflows/hil.yml).
It is manually triggered with a target ISA:

| Target | Required runner labels | Maximum runtime width |
| --- | --- | --- |
| `avx2` | `self-hosted`, `linux`, `x64`, `hil`, `avx2` | AVX2 |
| `avx512` | `self-hosted`, `linux`, `x64`, `hil`, `avx512` | AVX-512 |

The workflow contains three ordered stages:

1. **hardware-smoke** builds the project and runs [`ci/hw-smoke.sh`](../ci/hw-smoke.sh);
2. **stress-suite** exercises immutable width selection, signal pressure and
   telemetry fault recovery;
3. **thermal-characterization** runs fixed-width controls followed by a real
   registered adaptive workload for a selected 1–300 minute observation window.

The target is enforced by scripts, not inferred from a runner label.
[`ci/hil-preflight.sh`](../ci/hil-preflight.sh) rejects a machine without the
required CPU flag. The runtime is capped at the selected ISA, and final
validation requires application work to have executed at that width.

The last stage uploads `hil-artifacts/` even on failure. Each invocation writes
to a new child directory and refuses unsafe roots or reuse; it never recursively
clears a caller-provided artifact directory.

## Required runner capabilities

Run the preflight check under the same service account that executes the runner:

```bash
HIL_TARGET_ISA=avx2 ci/hil-preflight.sh
```

Use `HIL_TARGET_ISA=avx512` only on a host exposing `avx512f`. Preflight checks:

- Bash, CMake, C/C++ compilers, Make, curl, Git, grep, `lscpu`, `nproc`,
  Python 3 and `taskset`;
- the selected CPU ISA flag;
- at least two CPUs in the process affinity mask so workload and
  monitor/sampling activity can be separated;
- whether the configured metrics port can be bound.

It warns when temperature or RAPL sources are unavailable. Temperature is
required by the final characterization acceptance check. RAPL is optional and
must be reported as unavailable, never as zero. The runtime itself verifies
perf-event access and immutable trampoline mappings.

The normal permission target is `CAP_PERFMON` or an equivalent perf-event
policy. Do not add `CAP_SYS_ADMIN` as a generic workaround. If MSR-backed
APERF/MPERF is explicitly needed, grant only the narrow device access required
by that deployment.

An Intel Core i5-9500 is an appropriate AVX2 lane, not an AVX-512 lane. A local
characterization on that machine can be started with:

```bash
HIL_TARGET_ISA=avx2 SOAK_MINUTES=5 ci/thermal-soak.sh
```

Stop unrelated workloads, allow the machine to return to a repeatable idle
temperature and keep the governor/cooling setup unchanged between comparison
runs.

Useful overrides are `SOAK_MINUTES`, `METRICS_PORT`, `BUILD_DIR`,
`HIL_ARTIFACT_DIR`, `HIL_RUN_ID`, `HIL_WORK_ITEMS`, `HIL_CHUNK_ITEMS`,
`HIL_WORK_ROUNDS`, `HIL_BENCHMARK_TRIALS`, `HIL_BENCHMARK_SECONDS`,
`HIL_BENCHMARK_WARMUP_SECONDS`, `HIL_BENCHMARK_COOLDOWN_SECONDS`,
`HIL_BENCHMARK_SAMPLE_INTERVAL_SECONDS`, `HIL_PRE_SOAK_COOLDOWN_SECONDS` and
`HIL_ADAPTIVE_WARMUP_SECONDS`. Invalid or non-finite numeric values are rejected
before the build begins.

## Characterization procedure and artifacts

[`ci/thermal-soak.sh`](../ci/thermal-soak.sh) builds
`benchmark_registered_kernel`, then performs two distinct measurements:

1. repeated fixed SSE4.1 and AVX2 trials, plus AVX-512 on that lane, with
   alternating trial order, explicit warm-up and cooldown;
2. the registered kernel under the adaptive runtime while
   [`ci/hil_sampler.py`](../ci/hil_sampler.py) samples strict runtime health.

All kernel variants implement the same deterministic integer transform.
Checksums must match across fixed modes and the adaptive run. This prevents a
dispatch-only demo or unequal work from being presented as an SIMD speedup.

Each run directory includes:

- `machine-metadata.txt`: commit, toolchain, CPU/microcode, affinity, topology,
  governors and visible telemetry sources;
- `controlled-benchmark.json` and `.csv`: individual trials, medians, median
  absolute deviation, checksum, optional energy and efficiency ratios;
- `controlled-trials/`: stdout/stderr and result JSON for every fixed trial;
- `runtime.log` and `adaptive-workload.json`: adaptive registered-workload
  evidence and per-width work counts;
- `timeline.csv` and `health.jsonl`: sampled health/telemetry history;
- `metrics-final.prom`, `summary.json` and `summary.md`.

RAPL readings are wrap-aware. Mean power is computed from accumulated energy
over measured time rather than averaging instantaneous samples. The result is
still package-level energy, not energy attributable only to the process.

The automatic acceptance checks require:

- at least ten observations;
- at least 95% endpoint availability and runtime liveness;
- at least 90% strict readiness, hardware-perf coverage and package-temperature
  coverage;
- an observation of the selected width;
- application work executed at the selected width;
- at least one complete adaptive pass and graceful SIGTERM shutdown;
- matching checksums across adaptive and fixed controls;
- at least two fixed trials per required mode.

These are evidence-quality gates, not universal performance or thermal SLOs.
Temperature, power, throughput and acceptable variance depend on the CPU,
firmware, cooling, governor and application workload.

## Optional infrastructure-as-code assets

The `ci/hil/terraform` and `ci/hil/ansible` assets remain examples for teams
using the GitLab-compatible runner infrastructure. They currently describe an
AVX-512 lane. Cloud instance types and exposed CPU features change, so successful
provisioning does not prove the final VM satisfies the HIL contract. Verify the
machine before assigning labels or tags.

The Terraform example retrieves a modern `glrt-` runner authentication token
from an existing SSM SecureString, avoiding a plaintext token in Terraform state
and EC2 user data. Configure the `hil` and `avx512` tags when creating that
runner in GitLab. The example grants only `ssm:GetParameter` for the supplied
parameter ARN; a SecureString using a customer-managed KMS key also needs a
narrow `kms:Decrypt` grant for that key.

The GitLab-compatible entrypoint is [`ci/pipeline.yml`](../ci/pipeline.yml).
Set `HIL_TARGET_ISA` and give the runner the matching tag.

## Failure triage

### Hardware smoke

1. Run `HIL_TARGET_ISA=<target> ci/hw-smoke.sh` directly on the runner.
2. Confirm `lscpu` exposes the required flag under the runner's execution
   environment.
3. Check `perf stat -- sleep 1`, `perf_event_paranoid`, capabilities and process
   affinity under the same service user.
4. Check the runtime log for immutable mapping validation or startup failures.

### Stress suite

Reproduce the failing harness with bounded parameters:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target stress_patch_request stress_signal_storm stress_telemetry_faults
./build/stress_patch_request --threads=4 --iterations=1000
./build/stress_signal_storm --duration-seconds=10 --signal-rate=200
./build/stress_telemetry_faults --cycles=3
```

### Thermal characterization

1. Download the artifact even when the job failed and locate its per-run child
   directory.
2. Read `machine-metadata.txt` before interpreting numbers.
3. Inspect `controlled-benchmark.json` for checksum, trial variance and missing
   RAPL data.
4. Inspect `summary.json` for the failed coverage fraction.
5. Correlate `timeline.csv`, `health.jsonl` and `runtime.log` around failures.
6. If RAPL is absent, do not infer zero consumption.
7. Reproduce with a shorter window and the same target:

```bash
HIL_TARGET_ISA=avx2 SOAK_MINUTES=5 ci/thermal-soak.sh
```

## Release use

A workflow definition is not hardware evidence. Retain the artifact from the
exact release commit and record the CPU family, BIOS/firmware, kernel, governor
and cooling context. Repeat the characterization on materially different target
families; results from one AVX2 or AVX-512 machine do not generalize to every
machine exposing the same ISA.
