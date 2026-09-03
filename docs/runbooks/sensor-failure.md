# Runbook: Telemetry or Perf Failure

## Detection

A recoverable dependency failure normally keeps `/healthz` at HTTP 200 but
makes `/readyz` return 503. Inspect the health JSON rather than relying on
Prometheus counters that the current exporter does not expose:

```bash
curl --fail-with-body http://127.0.0.1:9464/healthz
curl --fail-with-body http://127.0.0.1:9464/readyz
```

Relevant fields are:

- `ready` and `controller.fallbackActive`;
- `perf.mode`, `perf.countersHealthy`, `perf.pinnedCpu` and `perf.monitorCpu`;
- `fusion.degraded`, `fusion.rawTempAvailable`,
  `fusion.filteredTempAvailable` and `fusion.freqAvailable`.

Logs use `event=perf_mode` and `event=telemetry_sensor` with degraded,
recovered and fail-closed reasons.

## Safety behavior

Hardware perf loss enters software/degraded mode and blocks wider upgrades by
default. Missing or stale raw temperature also blocks wider authorization.
Do not set `TSD_ALLOW_SOFTWARE_UPGRADES` during incident response; it is an
explicit override of the conservative behavior. Liveness remains healthy so
the runtime's bounded hardware re-probe can recover without an orchestrator
restart loop.

## Investigation

Run checks as the same user/capability set as the service:

```bash
cat /proc/sys/kernel/perf_event_paranoid
getcap /usr/local/bin/thermal_simd 2>/dev/null || true
find /sys/class/hwmon /sys/class/thermal -type f -readable 2>/dev/null | head
find /sys/devices/system/cpu -path '*/cpufreq/*_freq' -readable 2>/dev/null | head
```

Then verify:

1. the process affinity contains at least the intended workload and monitor
   CPUs;
2. `CAP_PERFMON` or the host perf-event policy reaches the actual service
   process/container;
3. the selected temperature and cpufreq files still exist and are readable;
4. a container has the required sysfs visibility;
5. suspend, CPU hotplug, kernel, firmware or VM migration did not change the
   exposed interface.

MSR absence alone is not fatal when cpufreq provides the frequency channel.
RAPL absence affects energy evidence, not runtime readiness.

## Recovery and verification

Fix the host permission/sysfs/affinity cause and allow the built-in re-probe to
run. Recovery is complete only after:

- `/readyz` returns 200 for a sustained observation window;
- `perf.mode` is `hardware` and `countersHealthy` is true;
- raw temperature and frequency remain available;
- logs show validated hardware recovery rather than merely a successful
  `perf_event_open` call.

Restart only when configuration or container mounts cannot be corrected in
place. After any restart, rerun `ci/hw-smoke.sh` under the service identity and
retain a short HIL artifact if the failure could affect release evidence.
