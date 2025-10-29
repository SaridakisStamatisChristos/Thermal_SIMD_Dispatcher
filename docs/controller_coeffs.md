# Controller Coefficient File

The predictive controller ingests coefficients from `config/controller_coeffs.json` (or the path provided via `--coeff-path`). The file is a JSON object with the following fields:

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

1. Update the JSON file on disk (e.g., write a new revision into the ConfigMap or local path).
2. Send `SIGHUP` to the dispatcher process. The controller marks a reload for the next control tick.
3. On the following recommendation cycle, the controller attempts to parse the file:
   - Success increments `predictive_coeff_reload_total` and logs an INFO entry with the new history window and staleness guard.
   - Failure increments `predictive_coeff_reload_errors_total`, logs an ERROR entry, and falls back to the previous coefficients or averaging forecast.

## Validation Tips

- Use `tests/policy/test_arx_model.cpp` as a reference for crafting deterministic coefficients during development.
- Monitor `predictive_abs_error_millic_total` to evaluate how well the updated coefficients track observed temperatures.
- Pair coefficient adjustments with updates to [Predictive Controller](predictive-controller.md) documentation to keep operational guidance in sync.
