# Hardware-in-the-Loop Continuous Integration

This document describes how the hardware-in-the-loop (HIL) continuous integration workflow provisions AVX-512 capable runners, executes stress workloads, and how operators can triage failures that surface during the pipeline.

## Runner Provisioning

### Terraform

The Terraform module under [`ci/hil/terraform`](../ci/hil/terraform) creates an AWS EC2 runner that exposes AVX-512 features and advertises the `hil` tag. Key inputs include the instance type (`var.instance_type`), AMI (`var.ami_id`), and GitLab registration metadata (`var.runner_registration_token`, `var.runner_coordinator_url`). The provisioning flow also generates a user-data bootstrap script that:

- Installs the build and profiling toolchain (CMake, Ninja, linux-tools).
- Registers the runner with the CI coordinator using supplied tags.
- Grants `CAP_PERFMON` and `CAP_SYS_ADMIN` to `/usr/bin/perf` and the `thermal_simd` binary so performance counters and AVX-512 trampolines can be exercised without root access.

To instantiate a runner:

```bash
cd ci/hil/terraform
terraform init
terraform apply -var "project=thermal-simd" -var "ami_id=ami-xxxxxxxx" -var "vpc_id=vpc-xxxxxxxx" -var "subnet_id=subnet-xxxxxxxx" -var "runner_registration_token=***" -var "runner_coordinator_url=https://gitlab.example.com"
```

The outputs report both public and private IP addresses for the runner.

### Ansible

After provisioning infrastructure, run the Ansible playbook under [`ci/hil/ansible`](../ci/hil/ansible) to harden the host:

```bash
ansible-galaxy collection install community.general
ansible-playbook -i ci/hil/ansible/inventory.ini ci/hil/ansible/playbooks/provision-runner.yml
```

The role verifies that `avx512f` is present in `lscpu`, installs Linux perf utilities, configures the runner service with ambient capabilities, and persists feature validation artifacts under `/etc/thermal-simd`.

## Pipeline Layout

The [CI pipeline configuration](../ci/pipeline.yml) introduces four stages:

1. **build** – Configures a Release build with testing enabled and produces the stress binaries.
2. **hw_smoke** – Invokes [`ci/hw-smoke.sh`](../ci/hw-smoke.sh) against an AVX-512 runner to validate baseline health checks.
3. **stress** – Executes the new stress harnesses to hammer the dispatcher with rapid patch requests, asynchronous signal storms, and telemetry fault injection.
4. **soak** – Runs [`ci/thermal-soak.sh`](../ci/thermal-soak.sh) for multi-hour loops to catch thermal regressions and intermittent failures.

Each hardware stage is pinned to runners tagged `hil` and `avx512` to ensure the capabilities negotiated above are present.

## Stress Harnesses

The `tests/stress` directory builds three dedicated binaries:

- `stress_patch_request` repeatedly patches SSE4.1/AVX2/AVX-512 trampolines with optional failure injection to vet CAP_PERFMON pathways.
- `stress_signal_storm` floods the runtime with `SIGUSR1`/`SIGUSR2` while patches execute, validating that the dispatcher remains stable under signal pressure.
- `stress_telemetry_faults` feeds malformed telemetry records into the monitor thread to exercise the fallback logic and ensure width stays at the safe baseline.

Each binary accepts CLI overrides (for example `--iterations`, `--duration-seconds`) so the pipeline can scale workloads for quick smoke runs or extended soaks.

## Failure Triage Runbooks

### Build or Provisioning Failures

1. Inspect Terraform logs for missing AVX-512 capable instance types or IAM permissions.
2. Confirm Ansible completed capability assignments by checking `/etc/systemd/system/gitlab-runner.service.d/capabilities.conf` and `/etc/thermal-simd/avx512.status` on the runner.
3. Rerun the Ansible playbook with `-vvv` to capture command-level diagnostics.

### Hardware Smoke Failures

1. Review the `hw-smoke` job output for CMake configuration or health check failures.
2. SSH into the runner and execute `ci/hw-smoke.sh` manually to reproduce.
3. Validate perf counters are accessible via `perf stat -- sleep 1`.
4. If AVX-512 detection fails, double check BIOS settings or cloud metadata for disabled instruction sets.

### Stress Harness Failures

1. Identify which binary failed by examining the `stress-suite` stage logs.
2. Reproduce locally via:
   ```bash
   cmake -S . -B build -DBUILD_TESTING=ON
   cmake --build build --target stress_signal_storm
   ./build/stress_signal_storm --duration-seconds 10 --signal-rate 200
   ```
3. For patch failures, verify `setcap` assignments on the trampoline binary and confirm `/proc/sys/kernel/perf_event_paranoid` is `1` or lower.
4. For signal storms, ensure the runner is not rate-limiting signals (for example inspect `/proc/sys/kernel/rate-limit`).

### Thermal Soak Regressions

1. Consult the `thermal-soak` stage for the round counter and elapsed time.
2. Download the job artifacts to analyze stress summaries over time.
3. SSH into the runner and launch `ci/thermal-soak.sh` with reduced `SOAK_MINUTES` to reproduce quickly.
4. Examine system metrics (temperature, throttling counters) via `sensors` and `dmesg` for hardware-side throttling events.

Following these runbooks standardizes incident response while ensuring the HIL fleet continuously validates AVX-512 thermal behavior.
