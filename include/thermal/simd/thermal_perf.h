#ifndef TSD_THERMAL_PERF_H
#define TSD_THERMAL_PERF_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include <thermal/simd/thermal_config.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct perf_ctx perf_ctx_t;

typedef enum {
    TSD_PERF_MODE_NONE = 0,
    TSD_PERF_MODE_HARDWARE,
    TSD_PERF_MODE_SOFTWARE
} tsd_perf_mode_t;

typedef struct {
    uint64_t cpi_milli;
    uint32_t ratio_milli;
    uint32_t trimmed_ratio_milli;
    uint64_t llc_mpki_milli;
    uint64_t severity_milli;
    int memory_bound;
} tsd_thermal_eval_t;

typedef void (*tsd_workload_fn)(void);

extern _Atomic uint64_t g_tsd_workload_iterations;

perf_ctx_t* tsd_perf_init(tsd_workload_fn workload_cb);
void tsd_perf_enable(perf_ctx_t *ctx);
void tsd_perf_cleanup(perf_ctx_t *ctx);
void tsd_perf_measure_baseline(perf_ctx_t *ctx, const tsd_runtime_config *cfg);
int tsd_perf_evaluate(perf_ctx_t *ctx, tsd_thermal_eval_t *out, const tsd_runtime_config *cfg);
tsd_perf_mode_t tsd_perf_get_mode(const perf_ctx_t *ctx);
int tsd_perf_get_pinned_cpu(const perf_ctx_t *ctx);
int tsd_perf_get_monitor_cpu(const perf_ctx_t *ctx);

#ifdef TSD_ENABLE_TESTS
void tsd_perf_set_fake_script(const uint32_t *ratios, size_t count, uint32_t mpki);
void tsd_perf_clear_fake_script(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TSD_THERMAL_PERF_H */
