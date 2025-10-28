#ifndef TSD_THERMAL_PERF_H
#define TSD_THERMAL_PERF_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include <thermal/simd/telemetry_helper.h>
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
    uint64_t thermal_severity_milli;
    int memory_bound;
    int temp_available;
    int freq_ratio_available;
    int32_t package_temp_millic;
    uint32_t freq_ratio_milli;
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
typedef enum {
    TSD_PERF_TEST_STEP_EINTR = 0,
    TSD_PERF_TEST_STEP_DATA
} tsd_perf_test_step_type_t;

typedef struct {
    tsd_perf_test_step_type_t type;
    size_t bytes;
} tsd_perf_test_read_step_t;

typedef struct {
    int fd;
    const tsd_perf_test_read_step_t *steps;
    size_t step_count;
    const uint8_t *data;
    size_t data_len;
} tsd_perf_test_read_stream_t;

void tsd_perf_set_fake_script(const uint32_t *ratios, size_t count, uint32_t mpki);
void tsd_perf_set_fake_telemetry(const tsd_telemetry_sample_t *samples, size_t count);
void tsd_perf_clear_fake_script(void);
perf_ctx_t* tsd_perf_test_create_dummy_context(void);
void tsd_perf_test_destroy_dummy_context(perf_ctx_t *ctx);
void tsd_perf_test_set_group_fd(perf_ctx_t *ctx, int fd);
void tsd_perf_test_set_llc_fd(perf_ctx_t *ctx, int fd);
void tsd_perf_test_set_mode(perf_ctx_t *ctx, tsd_perf_mode_t mode);
void tsd_perf_test_set_read_streams(const tsd_perf_test_read_stream_t *streams, size_t count);
void tsd_perf_test_clear_read_streams(void);
uint64_t tsd_perf_test_get_baseline_cpi(const perf_ctx_t *ctx);
uint64_t tsd_perf_test_get_baseline_mpki(const perf_ctx_t *ctx);
int tsd_perf_test_get_last_group_valid(const perf_ctx_t *ctx);
uint64_t tsd_perf_test_get_last_llc_value(const perf_ctx_t *ctx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TSD_THERMAL_PERF_H */
