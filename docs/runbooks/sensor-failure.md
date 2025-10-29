# Runbook: Telemetry Sensor Failure

## Summary
A mandatory telemetry sensor (perf counters, MSR temperature, or frequency source) is unavailable or returning invalid data, causing the dispatcher to downgrade SIMD width and possibly enter degraded mode.

## Detection
- Alert `telemetry_degraded_total` firing for >3 minutes.
- Logs containing `event=telemetry_sensor state=degraded` with `sensor=temp|perf|freq`.
- `/metrics` shows `telemetry_snapshots_total` increasing slower than the scheduler interval.

## Immediate Actions
1. **Confirm Scope**
   ```bash
   kubectl logs <pod> | grep telemetry_sensor | tail
   curl -s http://<pod>:9464/metrics | grep telemetry_degraded_total
   ```
2. **Force Health Check**
   ```bash
   kubectl exec <pod> -- ./thermal_simd --health-check
   ```
   - Exit code 0 ⇒ transient; continue monitoring.
   - Exit code ≠0 ⇒ remain degraded; proceed below.
3. **Stabilize Workload**
   - Set feature flag `TSD_FORCE_WIDTH=SSE41` via ConfigMap to minimize thermal stress until resolution.

## Remediation Steps
1. **Check Node Permissions**
   ```bash
   sudo sysctl kernel.perf_event_paranoid
   ls -l /dev/cpu/*/msr
   ```
   Ensure `perf_event_paranoid <= 1` and MSR device is readable by service account.
2. **Restart Telemetry Collector**
   ```bash
   systemctl restart tsd-telemetry.service
   ```
   or redeploy the pod (`kubectl rollout restart daemonset/thermal-simd`).
3. **Validate Sensor Output**
   ```bash
   sudo turbostat --Summary --show CoreTmp,CoreCnt
   ```
   Compare with telemetry logs to ensure the source is publishing valid data.
4. **Clear Degraded Mode**
   - Once sensors are stable for 5 minutes, remove `TSD_FORCE_WIDTH` override.
   - Confirm `/metrics` shows `telemetry_degraded_total` flat.

## Escalation
- If sensors remain unavailable for >30 minutes, escalate to platform engineering (PE On-Call) and attach sandbox artifacts (see `docs/sandbox-workflow.md`).
- For hardware-level failures, open a DC ops ticket referencing the host serial and attach turbostat output.

## Post-Incident
- File an incident report within 24h including root cause and mitigations.
- Update observability thresholds if the alert fired too late/early.
