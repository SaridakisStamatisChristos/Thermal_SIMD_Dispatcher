# Predictive Thermal Controller

The predictive controller combines reactive thermal throttling with a short-horizon forecast to minimize SIMD width flapping. It runs inside `controller/predictive_controller.c` and is invoked from the dispatcher event loop every scheduler interval.

## Goals
- Maintain CPU/package temperature below the configured ceiling without sacrificing SIMD throughput unnecessarily.
- Avoid oscillations caused by short thermal spikes by enforcing hysteresis and a minimum dwell time.
- Fail closed in the presence of missing data (telemetry dropouts, MSR inaccessibility) by downgrading SIMD width and escalating through the metrics pipeline.

## Control Inputs
| Signal | Source | Notes |
| --- | --- | --- |
| `thermal_cpi` | Derived from `perf_event_open` counters (`cycles` / `instructions`) | Sampled every interval and decayed with EWMA (`alpha=0.25`). |
| `package_temp_c` | MSR IA32_THERM_STATUS or `/sys/class/thermal/thermal_zone*` fallback | Normalized to Kelvin for forecast math; converted back for logging. |
| `freq_hint` | Telemetry fusion layer (see [Telemetry Fusion](telemetry-fusion.md)) | Indicates OEM turbo residency and informs forecast headroom. |
| `power_budget_w` | Optional RAPL reading | Drives predictive downgrade when dynamic power exceeds limit. |

Each signal is tagged with a monotonic timestamp. Stale signals (>2 intervals) are discarded and treated as unavailable.

## Forecast Model
The controller uses a single-step ARX/ARMAX model implemented in `src/policy/arx_model.cpp` and driven by coefficients stored in `config/controller_coeffs.json` (see [Controller Coefficients](controller_coeffs.md)). The model consumes a sliding window of recent telemetry samples and projects the next package temperature in millicelsius:

```
T[t+1] = bias
         + Σ φᵢ · T[t-i]
         + Σ θᵢ · Ratio[t-i]
         + Σ γᵢ · Severity[t-i]
         + ψ · ε[t]
```

- `φᵢ`, `θᵢ`, and `γᵢ` are configurable auto-regressive and exogenous coefficients.
- `ψ` is an optional moving-average gain applied to the most recent residual `ε[t] = T[t] - T̂[t]`.
- Missing temperature samples disable the prediction path and fall back to a simple moving average.
- Coefficient files support hot-reload: the controller listens for `SIGHUP` and re-reads `config/controller_coeffs.json` on the next control tick. Successful reloads and failures are logged and exported via metrics.

Telemetry freshness is enforced prior to forecasting. If the latest sample exceeds the configured `staleness_window_ms`, the controller skips predictive evaluation, logs a warning, and records `predictive_stale_samples_total`.

The forecast produces a projected temperature under the current SIMD width. The controller evaluates transitions (`SSE4.1`, `AVX2`, `AVX-512`) and selects the highest width whose projected temperature remains below `temp_ceiling_c - safety_margin_c` and whose CPI ratio is under `up_ratio`.

## Decision Pipeline
1. **Acquire Inputs:** Pull the latest telemetry fusion snapshot (all `TelemetrySnapshot` values share a generation number).
2. **Validate Freshness:** Reject snapshots older than `stale_threshold_ms`. Revert to downgrade path if stale.
3. **Run Forecast:** Compute `forecast_temp` and `forecast_cpi` using the ARX model.
4. **Evaluate Guards:**
   - If `forecast_temp >= temp_ceiling_c`, downgrade one SIMD level.
   - If `forecast_temp >= temp_ceiling_c + emergency_margin_c`, drop to scalar and set `state=emergency`.
   - Require `up_count` consecutive intervals below `up_ratio` before upgrading.
5. **Apply Dwell & Cooldown:** Enforce `min_dwell_ms` per width and cooldown timers between upgrades/downgrades.
6. **Actuate:** Program trampoline patch buffer, flip dispatch pointer, and log `event=controller_decision` with context fields.

## Configuration Knobs
| Flag | Description | Default |
| --- | --- | --- |
| `--temp-ceiling` | Maximum allowed package temperature (°C). | 92 |
| `--safety-margin` | Guard band subtracted from the ceiling for predictive upgrades (°C). | 4 |
| `--emergency-margin` | Additional buffer that triggers scalar fallback (°C). | 10 |
| `--forecast-horizon` | Number of intervals to project. Currently fixed at 1 but tunable for experiments. | 1 |
| `--coeff-path` | Override path to controller coefficients JSON. | `config/controller_coeffs.json` |
| `--predictive-alpha` | EWMA alpha applied to CPI history. | 0.25 |

## Telemetry & Metrics
- `predictive_forecasts_total`: ARX/ARMAX forecasts executed with valid telemetry.
- `predictive_decisions_total`: control decisions driven by the predictive controller.
- `predictive_abs_error_millic_total`: accumulated absolute prediction error in millicelsius.
- `predictive_stale_samples_total`: telemetry snapshots rejected due to staleness.
- `predictive_coeff_reload_total`: successful coefficient reloads (including on startup).
- `predictive_coeff_reload_errors_total`: failures to read or parse the coefficient file.

Metrics are exposed through the metrics subsystem documented in [Metrics Endpoints](metrics-endpoints.md).

## Failure Modes & Mitigations
| Failure | Detection | Mitigation |
| --- | --- | --- |
| Coefficient file missing/corrupt | Checksum verification on load | Log `level=error`, increment `predictive_coeff_reload_errors_total`, continue with baked-in safe coefficients. |
| Telemetry stall | Staleness guard > `stale_threshold_ms` | Force downgrade, escalate via `telemetry_stall` alert, set degraded mode bit. |
| Forecast divergence | `forecast_temp` deviates > `forecast_residual_threshold` for N intervals | Auto-revert to reactive controller and set `controller_state=reactive` until manual intervention. |

## Testing Strategy
- `tests/policy/test_policy_controller.c` validates coefficient application, dwell logic, and emergency fallbacks.
- Integration tests under `tests/smoke.sh` run with synthetic telemetry via `--health-check` to verify downgrades.
- CI pipeline (see README) executes these tests on every merge to `main` and is summarized in the [Validation Matrix](testing-matrix.md).
