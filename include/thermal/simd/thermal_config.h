#ifndef TSD_THERMAL_CONFIG_H
#define TSD_THERMAL_CONFIG_H

#include <stdint.h>

#include <thermal/simd/logging.h>
#include <config/policy_config.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int check_interval_us;
    int down_count;
    int up_count;
    double down_ratio;
    uint64_t down_ratio_milli;
    int cooldown_down_ms;
    int cooldown_up_ms;
    int allow_avx512;
    int min_dwell_ms;
    int memory_guard_divisor;
    int memory_guard_offset_milli;
    int demo_duration_sec;
    int run_forever;
    int work_iters;
    int cooldown_down_ticks;
    int cooldown_up_ticks;
    int min_dwell_ticks;
    int thermal_temp_weight_milli;
    int thermal_ratio_weight_milli;
    int degraded_timeout_sec;
    /* Compatibility/configuration field. Runtime degraded state is published
     * separately by tsd_runtime_config_is_degraded() and never mutates this
     * structure after startup. */
    int degraded_policy_active;
    int health_check_mode;
    int metrics_enabled;
    int metrics_port;
    char metrics_bind_host[64];
    char metrics_tls_cert_path[256];
    char metrics_tls_key_path[256];
    char metrics_tls_ca_path[256];
    int metrics_tls_require_client_auth;
    char metrics_basic_auth_user[128];
    char metrics_basic_auth_pass[128];
    char statsd_host[128];
    int statsd_port;
    int telemetry_interval_ms;
    int telemetry_max_skew_ms;
    double telemetry_ewma_alpha;
    char telemetry_profile_path[256];
    int predictive_temp_ceiling_c;
    int predictive_safety_margin_c;
    int predictive_emergency_margin_c;
    double predictive_alpha;
    char predictive_coeff_path[256];
    tsd_log_level_t log_level;
    tsd_policy_config policy;
} tsd_runtime_config;

/*
 * Legacy process-global configuration retained for source/ABI compatibility.
 * Configure it before starting the adaptive runtime; callers must not write it
 * concurrently with a live runtime. A validated immutable copy is captured for
 * each runtime generation, and runtime/control code consumes that copy rather
 * than live-reading this writable compatibility object.
 */
extern tsd_runtime_config g_tsd_config;

/*
 * Returns the immutable active runtime-generation configuration when a runtime
 * is live, otherwise the legacy startup configuration. The returned object is
 * read-only to callers and remains stable for the live generation.
 */
const tsd_runtime_config *tsd_runtime_config_active_snapshot(void);

/* The private runtime implementation is compiled with this definition. Route
 * its historical g_tsd_config reads through the immutable generation snapshot
 * without changing the external symbol or legacy source ABI. */
#ifdef TSD_RUNTIME_INTERNAL_IMPL
#define g_tsd_config (*tsd_runtime_config_active_snapshot())
#endif

/* Pure value initializers: these never modify process-global runtime state. */
void tsd_runtime_config_set_defaults(tsd_runtime_config *cfg);
int tsd_runtime_config_refresh_ticks(tsd_runtime_config *cfg);

/*
 * Dynamic degraded mode is one process-wide runtime overlay, deliberately
 * separate from configuration values. Only the active global runtime config
 * may enter/exit this state; initializing or editing an unrelated config value
 * can never alter a live runtime's conservative policy.
 */
void tsd_runtime_config_reset_dynamic_state(void);
void tsd_runtime_config_enter_degraded_mode(tsd_runtime_config *cfg, const char *reason);
void tsd_runtime_config_exit_degraded_mode(tsd_runtime_config *cfg, const char *reason);
int tsd_runtime_config_is_degraded(void);
int tsd_runtime_config_effective_down_count(const tsd_runtime_config *cfg);
int tsd_runtime_config_effective_up_count(const tsd_runtime_config *cfg);
int tsd_runtime_config_effective_cooldown_down_ms(const tsd_runtime_config *cfg);
int tsd_runtime_config_effective_cooldown_up_ms(const tsd_runtime_config *cfg);
int tsd_runtime_config_effective_min_dwell_ms(const tsd_runtime_config *cfg);
uint64_t tsd_runtime_config_effective_down_ratio_milli(const tsd_runtime_config *cfg);

int tsd_runtime_config_parse_cli(tsd_runtime_config *cfg, int argc, char **argv);
void tsd_runtime_config_print_usage(const char *prog);

#ifdef __cplusplus
}
#endif

#endif /* TSD_THERMAL_CONFIG_H */
