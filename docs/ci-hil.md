# Hardware-in-the-Loop Continuous Integration

This document describes the hardware-in-the-loop (HIL) validation path for AVX-512-capable Linux machines, the evidence produced by a run, and operator triage steps.

Hosted CI is intentionally not treated as hardware evidence. A runner should carry the `hil` and `avx512` labels only after operators have verified the actual CPU/VM exposes AVX-512 and the runtime has the perf/thermal permissions required for the intended test.

## GitHub Actions entrypoint

The canonical GitHub workflow is [`.github/workflows/hil.yml`](../.github/workflows/hil.yml). It is manually triggered and contains three ordered stages:

1. **hardware-smoke** — builds and executes [`ci/hw-smoke.sh`](../ci/hw-smoke.sh);
2. **stress-suite** — exercises immutable width selection, signal pressure and telemetry fault recovery;
3. **thermal-characterization** — runs [`ci/thermal-soak.sh`](../ci/thermal-soak.sh) and [`ci/hil_sampler.py`](../ci/hil_sampler.py) for a selected 1–300 minute observation window.

The final stage uploads `hil-artifacts/` even when characterization fails, so machine metadata and partial logs remain available for diagnosis.

## Required runner capabilities

At minimum, validate on the self-hosted runner:

```bash
lscpu | grep -i avx512
perf stat -- sleep 1
cat /proc/sys/kernel/perf_event_paranoid
```

The repository runtime itself will additionally verify its immutable trampoline mappings and hardware-counter behavior. Package-temperature telemetry must be visible through a supported thermal-zone or `hwmon` source for HIL characterization to pass its coverage threshold.

MSR access is optional for frequency telemetry because cpufreq can be used as a fallback. RAPL power is optional because some otherwise-valid systems do not expose package energy through Linux powercap.

Do not grant broad capabilities solely to satisfy a test if a narrower device ownership, service policy or `CAP_PERFMON` configuration is available.

## Optional infrastructure-as-code assets

The `ci/hil/terraform` and `ci/hil/ansible` assets remain available for teams using the repository's GitLab-compatible runner infrastructure. Cloud instance types and AVX-512 exposure change over time, so provisioning code is not proof that a resulting VM actually has the expected ISA. Verify the final machine before assigning HIL labels/tags.

The GitLab-compatible pipeline remains in [`ci/pipeline.yml`](../ci/pipeline.yml); GitHub users should prefer `.github/workflows/hil.yml`.

## Characterization artifacts

A successful `thermal-characterization` job records:

- exact Git commit and UTC start time;
- kernel and architecture;
- CPU model and microcode when exposed;
- process allowed-affinity mask;
- CPU package topology;
- cpufreq governors;
- visible thermal, powercap and MSR sources;
- runtime log;
- one-second-by-default CSV telemetry timeline;
- raw `/healthz` JSON snapshots in JSONL;
- final Prometheus snapshot;
- machine-readable summary JSON;
- Markdown summary copied into the GitHub Actions step summary.

The timeline includes current/recommended width, liveness/readiness, perf mode and counter health, workload/monitor CPUs, package temperature, frequency ratio, current sysfs CPU frequency and RAPL-derived package power when available.

The automatic acceptance checks require:

- at least ten observations;
- at least 95% health-endpoint availability;
- at least 95% runtime liveness;
- at least 90% validated hardware-perf coverage;
- at least 90% package-temperature coverage;
- at least one valid SIMD-width observation.

These are minimum evidence-quality checks, not universal thermal-performance SLOs. Deployment-specific temperature, power, throughput and oscillation limits should be evaluated separately.

## Stress harnesses

The `tests/stress` directory builds three dedicated binaries:

- `stress_patch_request` repeatedly requests immutable SSE4.1/AVX2/AVX-512 width selections with optional failure injection. The historical name contains `patch`, but production code pages are not rewritten at runtime.
- `stress_signal_storm` applies asynchronous signal pressure while width selections occur.
- `stress_telemetry_faults` exercises telemetry dropout and recovery behavior.

Each binary accepts CLI overrides so the workflow can scale workloads for quick smoke runs or extended validation.

## Failure triage

### Hardware smoke failure

1. Inspect the `hardware-smoke` log and the exact checked-out commit.
2. Run `ci/hw-smoke.sh` directly on the machine.
3. Verify `perf stat -- sleep 1` works under the same service user.
4. Verify `lscpu` still reports the expected ISA; cloud migration or BIOS/firmware changes can alter exposed features.
5. Check the runtime's immutable mapping self-validation output.

### Stress-suite failure

Reproduce the specific harness with bounded parameters first:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target stress_patch_request stress_signal_storm stress_telemetry_faults
./build/stress_patch_request --threads=4 --iterations=1000
./build/stress_signal_storm --duration-seconds=10 --signal-rate=200
./build/stress_telemetry_faults --cycles=3
```

For perf-related behavior, capture `perf_event_paranoid`, capabilities and the runtime's reported perf mode. For telemetry faults, inspect which package sensor was selected and whether recovery/backoff events were logged.

### Thermal characterization failure

1. Download the `thermal-simd-hil-<sha>` artifact even if the job failed.
2. Inspect `machine-metadata.txt` first to establish the CPU/kernel/governor/permission context.
3. Inspect `summary.json` for the failed coverage fraction.
4. Use `timeline.csv` to determine whether the issue was endpoint availability, perf fallback, missing temperature, or sustained degraded state.
5. Correlate `runtime.log` and `health.jsonl` around the failing timestamps.
6. If RAPL is present, inspect power changes around width transitions; if it is absent, do not infer zero power.
7. Reproduce with a shorter window:

```bash
SOAK_MINUTES=5 ci/thermal-soak.sh
```

## Release use

A HIL workflow definition is not itself evidence. For a release or a published hardware claim, retain the artifact from the exact release commit and identify the CPU family it represents. Repeat the characterization on materially different deployment families rather than assuming one AVX-512 machine generalizes to all AVX-512 systems.
