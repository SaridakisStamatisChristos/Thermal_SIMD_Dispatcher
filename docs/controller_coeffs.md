# Controller Coefficient File

The predictive controller ingests coefficients from `config/controller_coeffs.json`, the path provided via `--coeff-path`, or the `TSD_PREDICTIVE_COEFF_PATH` environment override. The environment override has highest precedence. The file is a JSON object with the following fields:

| Field | Type | Required | Description |
| --- | --- | --- | --- |
| `bias` | number | Yes | Constant term applied to the forecast (millicelsius). |
| `ar_temperature` | array<number> | Yes | Auto-regressive coefficients applied to historical package temperatures. The array length determines the minimum history window. |
| `ratio` | array<number> | No | Coefficients applied to historical SIMD ratio measurements (milli-units). |
| `trimmed_ratio` | array<number> | No | Coefficients applied to the trimmed ratio (if available). |
| `severity` | array<number> | No | Coefficients applied to the severity metric reported in telemetry (milli-units). |
| `ma` | number | No | Moving-average gain applied to the most recent residual (`actual - forecast`). |
| `staleness_window_ms` | number | No | Maximum age (in milliseconds) of telemetry used for prediction. Defaults to 500 ms. |

Example:

```json
{
  "bias": 1200.0,
  "ar_temperature": [0.85, 0.05],
  "ratio": [-0.30],
  "severity": [0.04],
  "ma": 0.25,
  "staleness_window_ms": 750
}
```

## Hot Reload Workflow

The library core does **not** install or replace process signal handlers. Embedders retain ownership of their signal model and can request reload through `tsd_dispatcher_policy_reload()`.

The standalone `thermal_simd` executable preserves the convenient Unix workflow:

1. Update the JSON file on disk (for example, write a new ConfigMap or local revision).
2. Send `SIGHUP` to the standalone dispatcher process.
3. The executable's minimal signal handler sets an atomic reload flag; file I/O and parsing happen later on the monitor thread through the explicit reload API.
4. Success increments `predictive_coeff_reload_total`; failure increments `predictive_coeff_reload_errors_total` and leaves the controller in its existing/fallback state.

This split keeps signal handling async-signal-safe while avoiding a library-level `std::signal()` side effect in embedding applications.

## Validation Tips

- Use `tests/policy/test_arx_model.cpp` as a reference for crafting deterministic coefficients during development.
- Monitor `predictive_abs_error_millic_total` to evaluate how well the updated coefficients track observed temperatures.
- Pair coefficient adjustments with updates to [Predictive Controller](predictive-controller.md) documentation to keep operational guidance in sync.
