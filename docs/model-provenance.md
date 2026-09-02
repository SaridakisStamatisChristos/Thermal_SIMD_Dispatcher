# Predictive model provenance

The predictive controller is intentionally separated from the runtime's fail-closed safety rules. This document defines what the shipped coefficient bundle means and, equally importantly, what it does **not** mean.

## Shipped coefficient bundle

`config/controller_coeffs.json` is the repository's conservative default/demo ARX parameter set. It exists so the predictive path is deterministic, testable and usable without an external calibration step.

It is **not** presented as a universally calibrated thermal model for every Intel/AMD CPU, firmware revision, cooling solution, kernel, governor, workload or ambient condition. A coefficient file that is suitable for one physical platform can be a poor plant model for another.

The runtime therefore keeps safety authority outside the model:

- raw package temperature drives emergency handling and wider-SIMD authorization;
- loss of required package-temperature telemetry blocks upgrades and can force SSE4.1;
- primary perf-counter loss enters software/degraded mode and blocks wider SIMD by default;
- predictive recommendations are bounded to the discrete ISA widths the host and runtime already permit;
- hysteresis remains available as the fallback controller.

A wrong ARX forecast can make the predictive policy less optimal, but it must not be able to bypass those independent safety invariants.

## What the policy tuner does

The repository's policy-tuner tool calibrates policy/SLO and penalty choices from archived observations. It does not claim to identify a physical ARX plant automatically and does not silently rewrite `controller_coeffs.json`.

Plant identification should be treated as a separate experimental procedure: collect representative time-aligned thermal/performance data, split fitting and validation windows, fit candidate coefficients, evaluate residuals and stability out of sample, then deploy the resulting bundle explicitly with `--coeff-path` or `TSD_PREDICTIVE_COEFF_PATH`.

## Reproducibility expectations for calibrated bundles

For a coefficient bundle intended to support a published benchmark or production deployment, retain at least:

- CPU model/stepping and microcode;
- socket/package topology;
- BIOS/firmware power configuration where relevant;
- kernel version;
- cpufreq governor and frequency limits;
- cooling configuration and ambient conditions when available;
- workload definition and dataset;
- telemetry sampling interval and freshness window;
- fitting/validation data identifiers;
- fitting method and objective;
- coefficient bundle checksum;
- validation error metrics and the range of temperatures/frequencies covered.

The manual HIL workflow records the machine/runtime side of this provenance and uploads time-series evidence. It does not automatically convert that evidence into a calibrated ARX model.

## Versioning

Coefficient semantics are part of the runtime contract. Changes to field meaning or model structure should be accompanied by a project/version change and migration documentation. Merely changing numerical values for a specific platform should be distributed as a separate coefficient bundle rather than silently redefining the repository default.
