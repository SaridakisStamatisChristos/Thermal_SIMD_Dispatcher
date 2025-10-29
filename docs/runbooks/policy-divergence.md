# Runbook: Policy Divergence

## Summary
Production nodes are reporting divergent SIMD policies (e.g., controllers running different coefficients, telemetry skew thresholds mismatched), leading to inconsistent throttling behavior across the fleet.

## Detection
- Fleet-level alert `predictive_policy_mismatch` firing from compliance dashboards.
- Logs with `event=policy_digest` showing different hashes across nodes.
- `/metrics` exposes `policy_reload_total` increasing unexpectedly.

## Immediate Actions
1. **Gather Evidence**
   ```bash
   kubectl get cm -n thermal-simd controller-policy -o yaml > /tmp/policy.yaml
   kubectl logs <pod> | grep policy_digest | tail
   ```
2. **Compare Hashes**
   ```bash
   sha256sum config/controller_coeffs.json
   ```
   Ensure it matches the blessed hash in release notes.

## Remediation Steps
1. **Lock Policy Config**
   - Set `featureFreeze=true` in the policy ConfigMap to prevent hot reloads:
     ```bash
     kubectl patch cm controller-policy -p '{"data":{"featureFreeze":"true"}}'
     ```
2. **Redeploy With Blessed Config**
   ```bash
   kubectl create configmap controller-policy --from-file=config/controller_coeffs.json --dry-run=client -o yaml | kubectl apply -f -
   kubectl rollout restart daemonset/thermal-simd
   ```
3. **Verify Alignment**
   ```bash
   kubectl exec <pod> -- ./tools/policy_digest --coeff-path /etc/tsd/controller_coeffs.json
   ```
   Compare digest output across multiple nodes.
4. **Monitor Metrics**
   - Confirm `predictive_policy_mismatch` alert clears.
   - Ensure `predictive_forecasts_total` growth resumes uniformly across nodes.

## Escalation
- If divergence persists, involve Release Engineering to audit artifact promotion pipeline.
- Notify compliance if divergence existed >2 hours to evaluate reporting obligations.

## Post-Incident
- Document root cause in the incident tracker with links to ConfigMap changes.
- Update `docs/predictive-controller.md` with any coefficient adjustments made.
- Add regression test covering policy reload scenario if missing.
