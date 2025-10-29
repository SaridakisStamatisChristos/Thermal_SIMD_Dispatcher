# Runbook: Patcher Attestation Alert

## Summary
The security attestation service flagged the dispatcher patcher subsystem due to failed measurement verification (hash mismatch, signature failure, or stale nonce). The dispatcher may refuse to patch or run with reduced capabilities.

## Detection
- Alert `patcher_attestation_failure` firing from security monitoring.
- Logs show `event=attestation state=failure reason=<...>`.
- `/metrics` contains `patch_failures_total` increasing and `attestation_verifications_total` spiking.

## Immediate Actions
1. **Confirm Alert Context**
   ```bash
   kubectl logs <pod> | grep attestation | tail
   curl -s http://<pod>:9753/metrics | egrep 'attestation|patch_failures_total'
   ```
2. **Check Dispatcher State**
   ```bash
   kubectl exec <pod> -- ./thermal_simd --health-check
   ```
   - Exit code 0 but logs show failure ⇒ patching suspended but runtime healthy.
   - Exit code ≠0 ⇒ runtime degraded; escalate quickly.

## Remediation Steps
1. **Validate Attestation Material**
   ```bash
   kubectl exec <pod> -- sha256sum /etc/tsd/patcher_measurement.json
   kubectl exec <pod> -- openssl dgst -sha256 -verify /etc/tsd/attestor_pub.pem -signature /run/tsd/nonce.sig /run/tsd/nonce.bin
   ```
   - Mismatch ⇒ rotate measurement bundle from artifact store.
2. **Refresh Nonce**
   ```bash
   kubectl exec <pod> -- ./tools/attestation_client --refresh-nonce
   ```
   Confirms that the dispatcher can fetch a fresh nonce and sign it.
3. **Redeploy Patcher**
   ```bash
   kubectl rollout restart daemonset/thermal-simd
   ```
   Ensures the trampoline patch buffer reinitializes with new attestation data.
4. **Coordinate With Security**
   - Notify security on-call with evidence (hashes, signature output).
   - Request whitelist update if the release contains intentional changes (attach diff + ticket).

## Escalation
- If attestation fails after bundle rotation, page Security Engineering immediately.
- Open a compliance incident ticket if the dispatcher runs in degraded mode for >1 hour.

## Post-Incident
- Update `docs/security/attestation.md` with new certificate fingerprints.
- Schedule a sandbox run (`make sandbox-attestation`) to capture fresh artifacts for future baselines.
