# Benchmarking

The repository separates deterministic software-overhead measurements from
hardware thermal characterization. The software benchmark does not claim a
thermal, energy, or application speedup.

Build and run the dispatch-overhead benchmark with:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DTSD_BUILD_BENCHMARKS=ON
cmake --build build-bench --target benchmark_dispatch -j
./build-bench/benchmark_dispatch 10000000
```

It compares a direct call with the v2 dispatch path using identical minimal
work, verifies identical checksums, and reports nanoseconds per call. Run it on
an otherwise idle system, repeat it several times, and report the median along
with CPU, compiler, flags, kernel, governor, and affinity.

Do not use this microbenchmark to infer throughput-per-watt or thermal benefit.
Those claims require the HIL procedure in [`ci-hil.md`](ci-hil.md) using real
registered kernels and retained provenance artifacts.

## Registered-kernel controls

`benchmark_registered_kernel` contains SSE4.1, AVX2 and AVX-512 variants of the
same deterministic integer-mix kernel. Every variant uses the public v2 kernel
shape, processes identical offsets and counts, and must produce the same FNV-1a
checksum. The fixed modes call each implementation directly; adaptive mode
registers all variants and re-resolves the live authorized width at bounded
chunk boundaries.

```bash
cmake --build build-bench --target benchmark_registered_kernel -j
./build-bench/benchmark_registered_kernel \
  --mode=avx2 --duration-seconds=5 --result-json=avx2.json
```

[`ci/controlled_benchmark.py`](../ci/controlled_benchmark.py) runs repeated
fixed-width trials. It alternates forward/reverse mode order to reduce temporal
bias, performs an independent warmup for each trial, reports medians plus median
absolute deviation, keeps the sampler off the pinned workload CPU when the
affinity mask contains at least two CPUs, and records package energy/temperature
when Linux exposes them. The HIL soak then runs the same kernel through adaptive
dispatch while collecting the full health timeline.

The included integer mix is a controlled dispatcher workload, not a proxy for
every application. Publish its results under that name and repeat validation
with deployment-specific registered kernels before making product-specific
speedup or efficiency claims.
