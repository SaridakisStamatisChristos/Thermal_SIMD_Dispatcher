# Software and Startup Sandbox Workflow

This repository uses “sandbox” in two concrete ways:

1. the executable's bounded startup diagnostic (`--sandbox-only` or
   `--health-check`);
2. the hosted [Sandbox Regression workflow](../.github/workflows/sandbox.yml),
   which runs deterministic policy/telemetry tests and forced software-perf
   behavior.

It does not contain a container orchestrator under `ci/sandbox`, an injectable
telemetry Unix socket, telemetry fuzzer, synthetic workload shared object or
metrics-probe tool.

## Startup diagnostic

Build and run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target thermal_simd -j
./build/thermal_simd --sandbox-only --metrics-port=0
```

The diagnostic checks the immutable trampoline mapping, initializes the perf
subsystem and requires a hardware perf mode plus at least one temperature or
frequency telemetry source. It exits after reporting success/failure. Run it
under the same user, capabilities, cpuset and sysfs mounts as the intended
service; running it as an administrator does not validate the service identity.

`--health-check` includes the same startup checks and exits with status. Normal
persistent startup also runs the sandbox and stays locked to SSE4.1 safe mode
when it fails.

## Deterministic software-perf path

`TSD_FAKE_PERF=1` is a development/test override that forces recoverable
software mode without changing host perf permissions:

```bash
TSD_FAKE_PERF=1 ./build/thermal_simd \
  --no-avx512 --duration-sec=1 --metrics-port=0
```

This validates fail-closed behavior, not hardware recovery or thermal
performance. Never present it as HIL evidence and do not configure it in
production.

## Hosted regression suite

Run the same bounded targets used by `.github/workflows/sandbox.yml`:

```bash
ctest --test-dir build --output-on-failure \
  -R 'telemetry|policy|runtime_config'
TSD_FAKE_PERF=1 ./build/thermal_simd \
  --no-avx512 --duration-sec=1 --work-iters=100000 --metrics-port=0
```

The broader CTest suite also includes telemetry-fusion concurrency and three
bounded stress executables. Their command-line options can be increased for
longer software stress, but they still do not replace bare-metal HIL.

## Evidence boundary

Hosted runs may establish parser, policy, lifecycle, concurrency and failover
invariants. Only [`ci/thermal-soak.sh`](../ci/thermal-soak.sh) on a suitable
self-hosted machine produces the target-aware perf/temperature/ISA evidence
described in [`ci-hil.md`](ci-hil.md).
