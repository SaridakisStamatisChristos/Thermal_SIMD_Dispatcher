# Thermal‑Aware Self‑Patching SIMD Dispatcher

Production‑grade, Linux x86‑64 only. Runtime chooses between SSE4.1 / AVX2 / AVX‑512 (XMM‑only) and self‑patches a tiny trampoline under strict W^X with a double buffer. Thermal adaptation uses time‑scaled CPI from `perf_event_open` with hysteresis, cooldown and a minimum dwell time. A small shim handles scalar↔SIMD and avoids AVX/SSE transition penalties.

## Build

### Make
```bash
make
```

### CMake
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

## Run

> Requires `CAP_PERFMON` or `sudo sysctl kernel.perf_event_paranoid=0`.

```bash
./thermal_simd --help
./thermal_simd --no-avx512 --interval=100 --down-ratio=1.3 --duration-sec=5
```

## Flags
- `--interval=MS` check interval (default 50)
- `--down-count=N` throttles before downgrade (default 3)
- `--up-count=N` stable intervals before upgrade (default 5)
- `--down-ratio=R` throttle threshold as CPI multiple (default 1.5)
- `--cooldown-down=MS` cooldown after downgrade (default 1000)
- `--cooldown-up=MS` cooldown after upgrade (default 2000)
- `--min-dwell=MS` minimum time per SIMD width (default 200)
- `--no-avx512` disable AVX‑512 usage
- `--duration-sec=S` runtime duration for demo (default 10)
- `--work-iters=N` inner work iterations per tick (default 10,000,000)

## Tests
Run smoke tests (build + basic run):
```bash
tests/compile.sh && tests/smoke.sh
```

## Notes
- Requires SSE4.1 (fails fast otherwise)
- Uses `perf_event_open`; in containers, add `--cap-add=SYS_ADMIN` or run privileged
- XMM‑only payloads to minimize downclocks and power
