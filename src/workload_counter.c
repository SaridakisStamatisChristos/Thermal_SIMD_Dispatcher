#include <thermal/simd/thermal_perf.h>

#include <stdatomic.h>

_Atomic(uint64_t) g_tsd_workload_iterations = 0;

uint64_t tsd_workload_iterations_load(void) {
    return atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
}

void tsd_workload_iterations_add(uint64_t count) {
    if (count == 0) return;
    (void)atomic_fetch_add_explicit(&g_tsd_workload_iterations, count, memory_order_relaxed);
}

void tsd_workload_iterations_reset(void) {
    atomic_store_explicit(&g_tsd_workload_iterations, 0, memory_order_relaxed);
}
