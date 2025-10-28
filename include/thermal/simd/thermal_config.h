#ifndef TSD_THERMAL_CONFIG_H
#define TSD_THERMAL_CONFIG_H

#include <stdint.h>

#include <thermal/simd/logging.h>

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
    int work_iters;
    int cooldown_down_ticks;
    int cooldown_up_ticks;
    int min_dwell_ticks;
    int thermal_temp_weight_milli;
    int thermal_ratio_weight_milli;
    int degraded_timeout_sec;
    int degraded_policy_active;
    int health_check_mode;
    tsd_log_level_t log_level;
} tsd_runtime_config;

extern tsd_runtime_config g_tsd_config;

void tsd_runtime_config_set_defaults(tsd_runtime_config *cfg);
int tsd_runtime_config_refresh_ticks(tsd_runtime_config *cfg);
void tsd_runtime_config_enter_degraded_mode(tsd_runtime_config *cfg, const char *reason);
void tsd_runtime_config_exit_degraded_mode(tsd_runtime_config *cfg, const char *reason);
int tsd_runtime_config_is_degraded(void);

#ifndef TSD_ENABLE_TESTS
int tsd_runtime_config_parse_cli(tsd_runtime_config *cfg, int argc, char **argv);
void tsd_runtime_config_print_usage(const char *prog);
#endif

#endif /* TSD_THERMAL_CONFIG_H */
