# Predictive Thermal Policy

The predictive path is implemented by `src/policy/mpc_controller.cpp` together with `src/policy/arx_model.cpp`. Despite the historical `MPCController` class name, the current implementation is a **model-assisted discrete candidate optimizer**, not a general receding-horizon MPC solver. It forecasts thermal state from recent telemetry, scores the available SIMD modes, and falls back to the reactive hysteresis path when predictive inputs are unavailable or stale.

## Goals

- Keep CPI/thermal behavior near configured service-level targets without unnecessary SIMD-mode flapping.
- Penalize width changes so small forecast differences do not trigger transitions.
- Reject stale telemetry and invalid model state.
- Preserve the independent reactive hysteresis controller as the conservative fallback.
- Never authorize a wider SIMD mode without current package-temperature headroom.

## Inputs

Each `TelemetrySample` stored by the predictive controller includes:

| Signal | Source | Use |
| --- | --- | --- |
| `ratio_milli` | CPI-derived performance ratio | Candidate cost and short-term trend. |
| `trimmed_ratio_milli` | Trimmed ratio history | More robust forecast ratio when available. |
| `severity_milli` | Reactive evaluator | ARX exogenous input. |
| `temperature_millic` | Fused package-temperature telemetry | ARX autoregressive input, temperature cost and upgrade guard. |
| current SIMD width | Dispatcher state | Transition-distance/penalty calculation. |
| monotonic timestamp | `steady_clock` | Staleness rejection. |

The current ARX temperature predictor does **not** model SIMD width as a fitted plant-control coefficient. Candidate-width effects are handled by the controller scoring function. This limitation is intentional and is why the implementation should not be described as full MPC.

## ARX / ARMAX temperature model

Coefficients are loaded from `--coeff-path`, `TSD_PREDICTIVE_COEFF_PATH`, or the build/installed default. The estimator is implemented in `src/policy/arx_model.cpp` and computes a one-step package-temperature estimate from recent samples:

```text
T_hat[t+1] = bias
           + sum(phi_i * T[t-i])
           + sum(theta_i * Ratio[t-i])
           + sum(gamma_i * Severity[t-i])
           + sum(delta_i * TrimmedRatio[t-i])
           + psi * residual[t]
```

The moving-average residual term is optional. If no valid temperature sample is available, the ARX prediction is rejected and the controller uses its fallback averaging path instead.

The coefficient file can also define `staleness_window_ms`. The library exposes explicit reload through `tsd_dispatcher_policy_reload()` and does not install a process-global signal handler. The standalone executable maps `SIGHUP` to that API on the next monitor tick.

## Candidate scoring

For each control tick, the controller considers the current width and each supported candidate up to the detected maximum:

- `SIMD_SSE41`
- `SIMD_AVX2`
- `SIMD_AVX512`

The score combines:

1. distance between projected ratio and `slo_ratio_milli`;
2. distance between projected temperature and `slo_temp_millic`;
3. an upgrade/downgrade transition penalty proportional to the number of width steps;
4. a stability margin and minimum-improvement requirement before changing width.

`predictive_alpha` is used as the short-term ratio-trend contribution. In addition to candidate cost, runtime temperature guards are enforced independently: upgrades require temperature at or below `temp_ceiling - safety_margin`, while a sample at or above `temp_ceiling + emergency_margin` requests the conservative SSE4.1 width.

This is a finite candidate-selection problem. The implementation does not optimize a sequence `u[t..t+H]` and does not roll a width-dependent plant model through a control horizon.

## Interaction with the dispatcher

1. `tsd_dispatcher_policy_record()` pushes each latest thermal evaluation into controller history and refreshes the controller heartbeat.
2. `tsd_dispatcher_policy_recommend()` asks for a candidate width.
3. If the predictive controller declines or is in fallback state, the runtime continues through the existing reactive hysteresis logic.
4. Any wider target is independently gated by sandbox state, current perf mode and current package-temperature headroom.
5. In software perf mode, wider SIMD is continuously prohibited and software mode never authorizes a wider target.
6. If a different width remains authorized, `tsd_trampoline_patch()` selects the corresponding **immutable RX trampoline**.
7. Cooldown and minimum-dwell constraints remain enforced by the dispatcher loop.

`tsd_trampoline_patch()` is a compatibility name: production transitions no longer rewrite executable bytes.

## Configuration

The policy-level defaults are defined in `src/policy/policy_config.c`:

| Setting | Default | Meaning |
| --- | ---: | --- |
| `slo_ratio_milli` | 1500 | Target CPI/performance ratio. |
| `slo_temp_millic` | 85000 | Target package temperature in millicelsius. |
| `transition_penalty_up_milli` | 750 | Cost of each upward SIMD step. |
| `transition_penalty_down_milli` | 1000 | Cost of each downward SIMD step. |
| `forecast_horizon` | 5 | Number of recent samples used by the policy/forecast helpers. |

Runtime CLI/configuration wiring is documented in [`configuration.md`](configuration.md) and [`controller_coeffs.md`](controller_coeffs.md).

## Metrics

The controller exports counters including:

- `predictive_forecasts_total`
- `predictive_decisions_total`
- `predictive_abs_error_millic_total`
- `predictive_stale_samples_total`
- `predictive_coeff_reload_total`
- `predictive_coeff_reload_errors_total`

These are observability signals, not proof that every transition was generated by the predictive path; the dispatcher can still act through the reactive fallback controller.

## Failure behavior

| Condition | Behavior |
| --- | --- |
| Coefficient file missing or malformed | Log reload failure, keep predictive path conservative/fallback-capable. |
| Latest telemetry older than staleness window | Reject predictive recommendation and increment stale-sample metric. |
| Missing valid temperature | Never authorize a wider SIMD mode; downgrades remain available. |
| Package temperature inside safety guard band | Block wider transition even if the cost model prefers it. |
| Emergency temperature threshold reached | Request SSE4.1 fail-closed target. |
| Patch/selection failure | Dispatcher forces predictive fallback and retains/re-enters the conservative path. |
| Hardware-perf loss | Enter software/degraded mode, constrain wider transitions unless explicitly overridden, and hot-reprobe hardware counters. |

## Validation

- `tests/policy/test_policy_controller.c` exercises policy behavior and fallback paths.
- `tests/policy/test_arx_model.cpp` validates coefficient parsing, forecasting, residual handling and explicit reload.
- `.github/workflows/sandbox.yml` runs policy/telemetry regressions and a forced software-perf runtime path.
- Hardware thermal behavior still requires the HIL/thermal-soak pipeline described in [`testing-matrix.md`](testing-matrix.md).

## Future step toward true MPC

A genuine MPC implementation would make SIMD mode (and ideally workload intensity/power) an explicit fitted control input to the plant model, identify per-platform dynamics, roll candidate trajectories across a horizon, and optimize a constrained objective such as throughput, temperature headroom and energy. The current code deliberately stops short of claiming that capability.
