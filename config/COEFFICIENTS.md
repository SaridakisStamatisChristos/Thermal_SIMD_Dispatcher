# Controller coefficient provenance

`controller_coeffs.json` ships as a conservative reference profile for the
model-assisted discrete SIMD-width controller. It is **not** claimed to be a
CPU-independent physical model and should not be interpreted as measured silicon
characterization.

The ARX terms provide a bounded forecast over recent telemetry. The fields
`width_temperature_millic_per_step` and `width_performance_benefit_milli_per_step`
make SIMD width an explicit control input. The default values are engineering
priors chosen to make widening thermally costly rather than allowing the optimizer
to infer that a wider width can cool the package from the sign of an SLO error.

For production use, calibrate these coefficients on each target CPU/package and
representative workload, validate them against held-out traces, and verify the
result with hardware-in-the-loop runs. Raw package temperature remains an
independent fail-closed safety signal and is never replaced by the model forecast.
