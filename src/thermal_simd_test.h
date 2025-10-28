#ifndef TSD_THERMAL_SIMD_TEST_H
#define TSD_THERMAL_SIMD_TEST_H

#include <stddef.h>
#include <stdint.h>

#include <thermal/simd/simd_width.h>
#include <thermal/simd/thermal_perf.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TSD_ENABLE_TESTS

typedef struct perf_ctx perf_ctx_t;

typedef enum {
    TSD_PATCH_FAIL_NONE = 0,
    TSD_PATCH_FAIL_PROTECT_WRITE,
    TSD_PATCH_FAIL_PROTECT_EXEC
} tsd_patch_fail_stage_t;

void tsd_test_reset_runtime(void);
void tsd_test_set_policy_counts(int down, int up);
void tsd_test_set_timing(int interval_us, int cooldown_down_ms, int cooldown_up_ms, int dwell_ms);
int tsd_test_refresh_ticks(void);
void tsd_test_set_running(int value);
void tsd_test_run_workload(int iterations);
void tsd_test_reset_workload_counter(void);
void tsd_test_set_detect_override(simd_width_t (*fn)(void));
void tsd_test_clear_detect_override(void);
void tsd_test_override_patch(simd_width_t width, const uint8_t *bytes, size_t len);
void tsd_test_clear_patch_overrides(void);
const uint8_t* tsd_test_patch_bytes(simd_width_t width, size_t *len);
void tsd_test_set_fake_perf_script(const uint32_t *ratios, size_t count, uint32_t mpki);
void tsd_test_clear_fake_perf_script(void);
const char* tsd_test_last_patch_error(void);
void tsd_test_force_patch_failure(tsd_patch_fail_stage_t stage);
simd_width_t tsd_test_current_width(void);
unsigned char tsd_test_last_patched_width(void);
simd_width_t tsd_test_detect_host_max(void);
int tsd_test_patch(simd_width_t width);
int tsd_test_inactive_page_writable(void);
perf_ctx_t* tsd_test_init_perf(void);
void tsd_test_measure_baseline(perf_ctx_t *ctx);
void tsd_test_cleanup_perf(perf_ctx_t *ctx);
perf_ctx_t* tsd_test_perf_create_dummy_context(void);
void tsd_test_perf_destroy_dummy_context(perf_ctx_t *ctx);
void tsd_test_perf_set_group_fd(perf_ctx_t *ctx, int fd);
void tsd_test_perf_set_llc_fd(perf_ctx_t *ctx, int fd);
void tsd_test_perf_set_mode(perf_ctx_t *ctx, tsd_perf_mode_t mode);
void tsd_test_perf_set_read_streams(const tsd_perf_test_read_stream_t *streams, size_t count);
void tsd_test_perf_clear_read_streams(void);
uint64_t tsd_test_perf_get_baseline_cpi(const perf_ctx_t *ctx);
uint64_t tsd_test_perf_get_baseline_mpki(const perf_ctx_t *ctx);
int tsd_test_perf_get_last_group_valid(const perf_ctx_t *ctx);
uint64_t tsd_test_perf_get_last_llc_value(const perf_ctx_t *ctx);
int init_double_buffer_trampoline(void);
void* thermal_monitor_thread(void *arg);

#endif /* TSD_ENABLE_TESTS */

#ifdef __cplusplus
}
#endif

#endif /* TSD_THERMAL_SIMD_TEST_H */
