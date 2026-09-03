#ifndef TSD_THERMAL_PERF_H
#define TSD_THERMAL_PERF_H

#ifndef __cplusplus
#include <stdatomic.h>
#endif
#include <stddef.h>
#include <stdint.h>

#include <thermal/simd/telemetry_helper.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_eval_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct perf_ctx perf_ctx_t;

typedef enum {
    TSD_PERF_MODE_NONE = 0,
    TSD_PERF_MODE_HARDWARE,
    TSD_PERF_MODE_SOFTWARE
} tsd_perf_mode_t;

typedef void (*tsd_workload_fn)(void);

/*
 * The raw counter is C11-owned. Keep the raw declaration C-only so C++ never
 * aliases a C _Atomic object as std::atomic. Cross-language callers use the C
 * ABI functions below.
 */
#ifndef __cplusplus
extern _Atomic(uint64_t) g_tsd_workload_iterations;
#endif
uint64_t tsd_workload_iterations_load(void);
void tsd_workload_iterations_add(uint64_t count);
void tsd_workload_iterations_reset(void);

perf_ctx_t* tsd_perf_init(tsd_workload_fn workload_cb);
void tsd_perf_enable(perf_ctx_t *ctx);
void tsd_perf_cleanup(perf_ctx_t *ctx);
void tsd_perf_measure_baseline(perf_ctx_t *ctx, const tsd_runtime_config *cfg);
int tsd_perf_evaluate(perf_ctx_t *ctx, tsd_thermal_eval_t *out, const tsd_runtime_config *cfg);
tsd_perf_mode_t tsd_perf_get_mode(const perf_ctx_t *ctx);
int tsd_perf_get_pinned_cpu(const perf_ctx_t *ctx);
int tsd_perf_get_monitor_cpu(const perf_ctx_t *ctx);
int tsd_perf_check_software_timeout(perf_ctx_t *ctx, int timeout_sec);

/*
 * Wider SIMD authorization must be checked continuously. Only validated,
 * currently healthy hardware perf mode may authorize an upgrade. Software mode
 * is degraded diagnostic/control telemetry only and is always fail-closed to
 * SSE4.1, regardless of legacy environment variables.
 */
int tsd_perf_upgrades_allowed(const perf_ctx_t *ctx);

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
int tsd_perf_test_group_progress_valid(uint64_t before_enabled,
                                       uint64_t before_running,
                                       uint64_t before_cycles,
                                       uint64_t before_insns,
                                       uint64_t after_enabled,
                                       uint64_t after_running,
                                       uint64_t after_cycles,
                                       uint64_t after_insns);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TSD_THERMAL_PERF_H */
