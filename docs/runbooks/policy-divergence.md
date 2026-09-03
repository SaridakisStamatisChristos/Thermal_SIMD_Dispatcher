# Runbook: Predictive Policy Divergence

## Detection

Use this runbook when equivalent nodes make materially different width
recommendations or forecasts under comparable workload and thermal conditions.
First rule out expected platform differences: CPU model/microcode, cooling,
governor, affinity, kernel perf policy and sensor selection all affect results.

The repository does not ship a fleet policy-digest service or
`predictive_policy_mismatch` alert. A deployment may build those controls around
the coefficient file and logs.

## Gather evidence

For every affected node retain:

- executable/version and exact Git commit;
- `sha256sum` of the active coefficient file;
- process arguments and `TSD_PREDICTIVE_COEFF_PATH`, if set;
- CPU, microcode, kernel, governor and affinity;
- `/healthz` snapshots and logs around each recommendation;
- raw safety temperature, filtered control temperature, frequency ratio and
  perf mode.

Do not compare only `currentWidth`: live safety guards may correctly clamp a
controller recommendation.

## Configuration and reload checks

The standalone executable loads coefficients at startup and handles SIGHUP as
an explicit reload request. An embedding application can call
`tsd_dispatcher_policy_reload`. A failed reload retains existing/fallback state
and emits a log; there is no ConfigMap signature or feature-freeze mechanism in
this repository.

```bash
sha256sum /etc/thermal-simd/controller_coeffs.json
dispatcher_pid=1234  # replace with the real PID
kill -HUP "${dispatcher_pid}"
journalctl -u thermal-simd --since '5 minutes ago' | grep -E 'coefficient|SIGHUP|policy'
```

Confirm every node resolves the same path and digest. Also check file
permissions and whether an orchestration layer projected different ConfigMap or
secret revisions.

## Recovery

1. Restore the intended, calibrated coefficient file from a trusted release.
2. Reload once or redeploy the affected instances.
3. Confirm logs report a successful load from the intended path.
4. Compare nodes again under controlled, equivalent telemetry and workload.
5. If divergence persists with identical inputs, reproduce with
   `test_policy_controller` and `test_arx_model`, then retain the health/log
   sequence for a code defect investigation.

Do not promote a coefficient change solely because nodes converge. Preserve
the model's calibration provenance and validate it on each deployment CPU
family as described in [`../model-provenance.md`](../model-provenance.md).
