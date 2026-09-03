# Security Threat Model

## Scope and trust boundaries

This model covers the dispatcher process, immutable trampoline table,
registered-kernel selection, local telemetry, controller coefficient file and
metrics listener. It describes controls implemented in this repository; image
signing, cluster admission, SIEM retention and remote attestation are deployment
responsibilities.

The process trusts:

- the installed executable and controller coefficient file;
- the host kernel's CPUID, affinity, perf-event and sysfs interfaces;
- operators allowed to change process arguments, environment, files or
  capabilities;
- application-provided registered kernel pointers and context lifetimes.

## Assets and threats

| Asset/surface | Threat | Implemented control | Remaining responsibility |
| --- | --- | --- | --- |
| Trampoline table | Writable/executable memory or a corrupted indirect target | The whole table is built RW/non-executable, sealed RX, verified through `/proc/self/maps`, and never rewritten in production. Every target starts with `ENDBR64`. | Protect the process and binary from a same-privilege debugger or arbitrary memory-write primitive. |
| Width selection | Executing an ISA unsupported by the CPU/OS or wider than policy allows | CPUID/XCR0 checks, configured AVX-512 opt-in, immutable slot bounds and live perf/temperature authorization. SSE4.1 is fail-closed. | Register only correct, equivalent application kernels and honor context synchronization rules. |
| Local slot digest | Reporting a digest that races a width transition | Digest reads and selection are serialized; SHA-256 covers the active immutable slot. | The local digest is not a signature or remote attestation. An embedder must provision and authenticate any expected digest. |
| Perf/thermal telemetry | Missing, stale or invalid readings leading to unsafe upgrades | Freshness checks, hardware-counter validation, raw-temperature upgrade guard, safe-width fallback and bounded re-probe. | Secure the kernel/hardware telemetry path; privileged host compromise can falsify it. |
| Coefficient file | Malicious or unsuitable policy after startup/SIGHUP | Strict parser, bounded values, explicit reload API/SIGHUP handling, and retention of prior/fallback state on reload failure. | Restrict file write access and promote calibrated files with external provenance controls. |
| Metrics listener | Information disclosure, credential guessing or slow-client starvation | Loopback default, optional TLS 1.2+/mTLS/Basic Auth, constant-time credential comparison, bounded queue/workers/request size and client deadlines. | Do not expose plain unauthenticated HTTP beyond a trusted boundary; apply network policy and secret management. |
| Logs and HIL artifacts | Host/topology/thermal data disclosure or tampering | New per-run artifact directories and no recursive clearing of caller paths. | Restrict access, retention and upload destinations; sign artifacts externally if required. |

## Local trampoline measurement

[`include/patcher/attestation.h`](../../include/patcher/attestation.h) exposes:

- `tsd_attestation_get_active_hash`;
- `tsd_attestation_get_active_hash_hex`;
- `tsd_attestation_expect_active_hash`;
- `tsd_attestation_last_error`.

The measurement is updated after a successful immutable-slot selection. A
mismatch is returned to the caller and covered by the trampoline security test.
There is no manifest loader, signature verifier, nonce protocol, enclave key,
periodic remote verifier or attestation network service in this codebase.

## Abuse cases

### Unprivileged local workload

An unprivileged peer may contend for CPU/cache resources or influence the
thermal environment. The dispatcher can react conservatively, but it cannot
attribute contention to a tenant or guarantee isolation. Use cpusets, scheduler
controls and workload-specific SLOs at deployment level.

### Compromised metrics client

Metrics and health endpoints are read-only. A client can still consume bounded
connection slots or collect operational data. Keep the listener on loopback or
use TLS/auth plus network policy. The bounded queue and deadlines reduce, but do
not eliminate, denial-of-service risk.

### Compromised operator or host kernel

An actor able to replace the binary, change arguments/configuration, ptrace the
process or falsify kernel telemetry is inside the trust boundary. Use external
artifact signing, measured boot, admission controls and least-privilege access
where the deployment requires them; these are not supplied by this library.

## Verification

Hosted tests cover RX-only mappings, CET landing pads, local digest mismatch,
host/policy width bounds, perf failover/recovery, raw-temperature authorization,
metrics TLS/auth and slow clients. Bare-metal HIL is still required for actual
perf permissions, sensors, thermal behavior and ISA execution on each target
CPU family.

## Residual risks

- microarchitectural side channels and thermal/frequency coupling between
  colocated workloads;
- kernel or firmware telemetry errors below the process trust boundary;
- denial of service through resource exhaustion outside exporter limits;
- incorrect or semantically unequal application-registered kernels;
- calibration error in deployment-specific controller coefficients;
- local digest verification without an externally authenticated expected value.
