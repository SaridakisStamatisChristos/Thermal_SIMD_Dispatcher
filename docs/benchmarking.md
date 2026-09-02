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
