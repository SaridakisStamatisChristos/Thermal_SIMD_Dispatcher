# Security & Compliance: Threat Model and Attestation Procedures

## Scope
Covers the SIMD dispatcher runtime, predictive controller, telemetry collectors, and packaging artifacts (systemd unit, container image, Kubernetes manifests).

## Assets
- Trampoline patch buffers and runtime code sections (integrity critical).
- Telemetry data (temperature, frequency, power readings).
- Controller policies (`controller_coeffs.json`).
- Metrics endpoint and health check.
- Attestation material (measurement JSON, certificates, nonce).

## Threat Actors
| Actor | Capability | Goals |
| --- | --- | --- |
| Malicious tenant workload | Userland code within same host | Escalate privileges, manipulate SIMD decisions to degrade co-tenants. |
| Compromised operator | Access to Kubernetes control plane | Deploy tampered binaries or disable attestation. |
| Network attacker | Lateral movement within cluster | Scrape metrics or inject false telemetry. |
| Rogue hardware | Faulty sensors/MSRs | Cause unsafe temperature operation or force scalar mode. |

## Attack Surfaces & Mitigations
| Surface | Threat | Mitigation |
| --- | --- | --- |
| Patcher double buffer | Code injection | W^X enforced, buffers sealed with `mprotect`, attestation verifies measurement. |
| Telemetry socket (`/run/tsd/telemetry.sock`) | Unauthorized writes | Unix socket ACL restricted to dispatcher UID; sandbox fuzzer uses signed token. |
| Metrics endpoint | Data exfiltration | Binds to localhost; TLS + basic auth required for remote scraping. |
| Config hot reload | Policy tampering | ConfigMap signed with release key; hash compared to baseline (`policy_digest`). |
| Container image | Supply chain | Image signed via cosign; CI enforces signature verification before deploy. |

## Attestation Verification Procedure
1. **Measurement Baseline**
   - `packaging/artifacts/patcher_measurement.json` contains SHA256 of patch buffers and controller binaries.
   - Baseline is versioned per release and stored in artifact registry (see `packaging/README.md`).
2. **Startup Verification**
   - On boot, dispatcher loads measurement file, verifies signature with `attestor_pub.pem`.
   - Generates a nonce from the attestation service; signs it with enclave key.
   - Submits signed nonce + measurement digest to attestation API.
   - Attestation service returns `OK` with `expires_at` timestamp. Failure sets `state=degraded`.
3. **Runtime Checks**
   - Every `attestation_interval` (default 10m), dispatcher re-validates measurement and nonce freshness.
   - Metrics `attestation_verifications_total` and `attestation_failure_total` track results.
   - Structured logs `event=attestation` include `nonce_age_ms`, `result`, `digest`.
4. **Operator Verification**
   - Operators can run `tools/attestation_client --verify` to fetch current status:
     ```bash
     kubectl exec <pod> -- ./tools/attestation_client --verify
     ```
   - Output must show `result=OK` and digest matching release notes.
5. **Incident Response**
   - If attestation fails, follow `docs/runbooks/patcher-attestation-alert.md`.
   - Compliance requires documenting failure, timestamps, and remediation in ticketing system within 4 hours.

## Compliance Controls
- **CI Gate:** `ci/security.yml` runs SLSA provenance verification and cosign checks.
- **Runtime Logging:** Attestation and policy events stream to SIEM with 7-year retention.
- **Access Control:** Only the `sd-release` role can update measurement bundles via signed commits.
- **Change Management:** Threat model reviewed quarterly; updates require approval from Security Engineering + Compliance.

## Residual Risks
- Hardware sensor spoofing below firmware level (mitigated by redundant sensors when available).
- Side-channel leakage via SIMD width transitions (mitigated with fixed-time dispatchers in high-sensitivity tenants).

## Review History
| Date | Reviewer | Notes |
| --- | --- | --- |
| 2024-04-12 | Security Engineering | Initial model created for v1.3. |
| 2024-07-22 | Compliance | Added attestation expiry controls. |
| 2025-03-05 | Platform + Security | Updated for predictive controller rollout and telemetry fusion. |
