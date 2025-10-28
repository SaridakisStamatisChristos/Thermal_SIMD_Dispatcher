#include <thermal/simd/thermal_config.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <thermal/simd/config_parser.h>

#define LOG_COMPONENT "config"

static const tsd_policy_config k_default_policy_config = {
    .slo_ratio_milli = 1500,
    .slo_temp_millic = 85000,
    .transition_penalty_up_milli = 750,
    .transition_penalty_down_milli = 1000,
    .forecast_horizon = 5,
};

static const tsd_runtime_config k_default_config = {
    .check_interval_us = 50000,
    .down_count = 3,
    .up_count = 5,
    .down_ratio = 1.5,
    .down_ratio_milli = 1500,
    .cooldown_down_ms = 1000,
    .cooldown_up_ms = 2000,
    .allow_avx512 = 0,
    .min_dwell_ms = 200,
    .memory_guard_divisor = 5,
    .memory_guard_offset_milli = 200,
    .demo_duration_sec = 10,
    .work_iters = 10000000,
    .cooldown_down_ticks = 0,
    .cooldown_up_ticks = 0,
    .min_dwell_ticks = 0,
    .thermal_temp_weight_milli = 0,
    .thermal_ratio_weight_milli = 0,
    .degraded_timeout_sec = 120,
    .degraded_policy_active = 0,
    .health_check_mode = 0,
    .log_level = TSD_LOG_LEVEL_INFO,
    .policy = {0},
};

tsd_runtime_config g_tsd_config;

static tsd_runtime_config g_tsd_degraded_backup;
static int g_tsd_degraded_active = 0;

void tsd_runtime_config_set_defaults(tsd_runtime_config *cfg) {
    if (!cfg) {
        return;
    }
    *cfg = k_default_config;
    cfg->policy = k_default_policy_config;
    tsd_policy_config_apply_bounds(&cfg->policy);
    cfg->degraded_policy_active = 0;
    g_tsd_degraded_active = 0;
    memset(&g_tsd_degraded_backup, 0, sizeof(g_tsd_degraded_backup));
}

static void log_policy_change(const tsd_runtime_config *cfg, const char *reason, const char *state) {
    if (!cfg) {
        return;
    }
    tsd_log_warn(LOG_COMPONENT,
                 "event=policy_state state=%s reason=%s down_count=%d up_count=%d cooldown_down_ms=%d cooldown_up_ms=%d min_dwell_ms=%d down_ratio_milli=%" PRIu64,
                 state ? state : "unknown",
                 reason ? reason : "unknown",
                 cfg->down_count,
                 cfg->up_count,
                 cfg->cooldown_down_ms,
                 cfg->cooldown_up_ms,
                 cfg->min_dwell_ms,
                 cfg->down_ratio_milli);
}

void tsd_runtime_config_enter_degraded_mode(tsd_runtime_config *cfg, const char *reason) {
    if (!cfg) {
        return;
    }
    if (g_tsd_degraded_active) {
        return;
    }
    g_tsd_degraded_backup = *cfg;
    g_tsd_degraded_active = 1;
    cfg->degraded_policy_active = 1;
    if (cfg->down_count > 1) {
        cfg->down_count = cfg->down_count - 1;
    }
    if (cfg->up_count < 10) {
        cfg->up_count = cfg->up_count + 2;
    }
    if (cfg->cooldown_down_ms < 2000) {
        cfg->cooldown_down_ms = 2000;
    } else {
        cfg->cooldown_down_ms *= 2;
    }
    if (cfg->cooldown_up_ms < 4000) {
        cfg->cooldown_up_ms = 4000;
    } else {
        cfg->cooldown_up_ms *= 2;
    }
    if (cfg->min_dwell_ms < 500) {
        cfg->min_dwell_ms = 500;
    }
    if (cfg->down_ratio_milli > 1300) {
        cfg->down_ratio_milli = 1300;
        cfg->down_ratio = (double)cfg->down_ratio_milli / 1000.0;
    }
    tsd_runtime_config_refresh_ticks(cfg);
    log_policy_change(cfg, reason, "degraded");
}

void tsd_runtime_config_exit_degraded_mode(tsd_runtime_config *cfg, const char *reason) {
    if (!cfg) {
        return;
    }
    if (!g_tsd_degraded_active) {
        return;
    }
    *cfg = g_tsd_degraded_backup;
    cfg->degraded_policy_active = 0;
    g_tsd_degraded_active = 0;
    tsd_runtime_config_refresh_ticks(cfg);
    log_policy_change(cfg, reason, "recovered");
}

int tsd_runtime_config_is_degraded(void) {
    return g_tsd_degraded_active;
}

int tsd_runtime_config_refresh_ticks(tsd_runtime_config *cfg) {
    if (!cfg) {
        errno = EINVAL;
        return -1;
    }
    long long raw_ticks = 0;
    if (tsd_compute_ticks_from_ms(cfg->check_interval_us, cfg->cooldown_down_ms,
                                  &cfg->cooldown_down_ticks, &raw_ticks) != 0) {
        if (errno == EINVAL) {
            tsd_log_error(LOG_COMPONENT, "Invalid sampling interval while processing %s", "--cooldown-down");
        } else {
            tsd_log_error(LOG_COMPONENT, "Value for %s results in unsupported tick count (%lld)",
                          "--cooldown-down", raw_ticks);
        }
        return -1;
    }
    if (tsd_compute_ticks_from_ms(cfg->check_interval_us, cfg->cooldown_up_ms,
                                  &cfg->cooldown_up_ticks, &raw_ticks) != 0) {
        if (errno == EINVAL) {
            tsd_log_error(LOG_COMPONENT, "Invalid sampling interval while processing %s", "--cooldown-up");
        } else {
            tsd_log_error(LOG_COMPONENT, "Value for %s results in unsupported tick count (%lld)",
                          "--cooldown-up", raw_ticks);
        }
        return -1;
    }
    if (tsd_compute_ticks_from_ms(cfg->check_interval_us, cfg->min_dwell_ms,
                                  &cfg->min_dwell_ticks, &raw_ticks) != 0) {
        if (errno == EINVAL) {
            tsd_log_error(LOG_COMPONENT, "Invalid sampling interval while processing %s", "--min-dwell");
        } else {
            tsd_log_error(LOG_COMPONENT, "Value for %s results in unsupported tick count (%lld)",
                          "--min-dwell", raw_ticks);
        }
        return -1;
    }
    return 0;
}

#ifndef TSD_ENABLE_TESTS
static void die_invalid_option(const char *option, const char *value) {
    tsd_log_error(LOG_COMPONENT, "Invalid value for %s: '%s'", option, value ? value : "");
    exit(1);
}

void tsd_runtime_config_print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Options:\n");
    printf("  --interval=MS          Check interval in milliseconds (default: 50)\n");
    printf("  --down-count=N         Throttle events before downgrade (default: 3)\n");
    printf("  --up-count=N           Stable events before upgrade (default: 5)\n");
    printf("  --down-ratio=R         CPI ratio for throttle detection (default: 1.5)\n");
    printf("  --cooldown-down=MS     Cooldown after downgrade (default: 1000)\n");
    printf("  --cooldown-up=MS       Cooldown after upgrade (default: 2000)\n");
    printf("  --min-dwell=MS         Minimum time per width (default: 200)\n");
    printf("  --allow-avx512         Permit AVX-512 (default: disabled)\n");
    printf("  --no-avx512            Explicitly disable AVX-512\n");
    printf("  --memory-guard-div=N   Memory guard divisor [1-1000] (default: 5)\n");
    printf("  --memory-guard-offset=M Additional memory guard in milli-ratio [0-1000000] (default: 200)\n");
    printf("  --thermal-temp-weight=W Temperature severity weight in milli [0-100000] (default: 0)\n");
    printf("  --thermal-ratio-weight=W Frequency ratio severity weight in milli [0-100000] (default: 0)\n");
    printf("  --duration-sec=S       Demo duration (default: 10)\n");
    printf("  --work-iters=N         Inner work iterations per second (default: 10000000)\n");
    printf("  --degraded-timeout-sec=S Fail closed if hardware counters missing for S seconds (default: %d)\n",
           k_default_config.degraded_timeout_sec);
    printf("  --policy-slo-ratio=R   Predictive policy CPI target ratio (default: %.3f)\n",
           (double)k_default_policy_config.slo_ratio_milli / 1000.0);
    printf("  --policy-slo-temp=C    Predictive policy package temperature target in Celsius (default: %.1f)\n",
           (double)k_default_policy_config.slo_temp_millic / 1000.0);
    printf("  --policy-penalty-up=M  Predictive policy upgrade penalty in milli-cost (default: %u)\n",
           k_default_policy_config.transition_penalty_up_milli);
    printf("  --policy-penalty-down=M Predictive policy downgrade penalty in milli-cost (default: %u)\n",
           k_default_policy_config.transition_penalty_down_milli);
    printf("  --policy-forecast=N    Predictive policy forecast horizon in samples (default: %u)\n",
           k_default_policy_config.forecast_horizon);
    printf("  --health-check         Run diagnostics and exit with status\n");
    printf("  --log-level=LEVEL      Log verbosity (error, warn, info, debug)\n");
    printf("  --help                 Show this help\n");
}

int tsd_runtime_config_parse_cli(tsd_runtime_config *cfg, int argc, char **argv) {
    if (!cfg) {
        return -1;
    }
    tsd_runtime_config_set_defaults(cfg);

    const char *env_level = getenv("TSD_LOG_LEVEL");
    if (env_level && env_level[0] != '\0') {
        tsd_log_level_t parsed_env_level;
        if (tsd_log_level_from_string(env_level, &parsed_env_level) == 0) {
            cfg->log_level = parsed_env_level;
        } else {
            tsd_log_warn(LOG_COMPONENT,
                         "Ignoring invalid TSD_LOG_LEVEL '%s' (expected error|warn|info|debug)",
                         env_level);
        }
    }

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--interval=", 11)) {
            if (tsd_parse_ms_option(argv[i] + 11, 1, 10000, &cfg->check_interval_us) != 0) {
                die_invalid_option("--interval", argv[i] + 11);
            }
        } else if (!strncmp(argv[i], "--down-count=", 13)) {
            if (tsd_parse_int_option(argv[i] + 13, 1, 100, &cfg->down_count) != 0) {
                die_invalid_option("--down-count", argv[i] + 13);
            }
        } else if (!strncmp(argv[i], "--up-count=", 11)) {
            if (tsd_parse_int_option(argv[i] + 11, 1, 100, &cfg->up_count) != 0) {
                die_invalid_option("--up-count", argv[i] + 11);
            }
        } else if (!strncmp(argv[i], "--down-ratio=", 13)) {
            if (tsd_parse_ratio_option(argv[i] + 13, 1.0, 10.0, &cfg->down_ratio, &cfg->down_ratio_milli) != 0) {
                die_invalid_option("--down-ratio", argv[i] + 13);
            }
        } else if (!strncmp(argv[i], "--cooldown-down=", 16)) {
            if (tsd_parse_int_option(argv[i] + 16, 1, 3600000, &cfg->cooldown_down_ms) != 0) {
                die_invalid_option("--cooldown-down", argv[i] + 16);
            }
        } else if (!strncmp(argv[i], "--cooldown-up=", 14)) {
            if (tsd_parse_int_option(argv[i] + 14, 1, 3600000, &cfg->cooldown_up_ms) != 0) {
                die_invalid_option("--cooldown-up", argv[i] + 14);
            }
        } else if (!strncmp(argv[i], "--min-dwell=", 12)) {
            if (tsd_parse_int_option(argv[i] + 12, 1, 3600000, &cfg->min_dwell_ms) != 0) {
                die_invalid_option("--min-dwell", argv[i] + 12);
            }
        } else if (!strcmp(argv[i], "--no-avx512")) {
            cfg->allow_avx512 = 0;
        } else if (!strcmp(argv[i], "--allow-avx512")) {
            cfg->allow_avx512 = 1;
        } else if (!strncmp(argv[i], "--memory-guard-div=", 20)) {
            if (tsd_parse_int_option(argv[i] + 20, 1, 1000, &cfg->memory_guard_divisor) != 0) {
                die_invalid_option("--memory-guard-div", argv[i] + 20);
            }
        } else if (!strncmp(argv[i], "--memory-guard-offset=", 23)) {
            if (tsd_parse_int_option(argv[i] + 23, 0, 1000000, &cfg->memory_guard_offset_milli) != 0) {
                die_invalid_option("--memory-guard-offset", argv[i] + 23);
            }
        } else if (!strncmp(argv[i], "--thermal-temp-weight=", 22)) {
            if (tsd_parse_int_option(argv[i] + 22, 0, 100000, &cfg->thermal_temp_weight_milli) != 0) {
                die_invalid_option("--thermal-temp-weight", argv[i] + 22);
            }
        } else if (!strncmp(argv[i], "--thermal-ratio-weight=", 23)) {
            if (tsd_parse_int_option(argv[i] + 23, 0, 100000, &cfg->thermal_ratio_weight_milli) != 0) {
                die_invalid_option("--thermal-ratio-weight", argv[i] + 23);
            }
        } else if (!strncmp(argv[i], "--duration-sec=", 15)) {
            if (tsd_parse_int_option(argv[i] + 15, 1, 86400, &cfg->demo_duration_sec) != 0) {
                die_invalid_option("--duration-sec", argv[i] + 15);
            }
        } else if (!strncmp(argv[i], "--work-iters=", 13)) {
            if (tsd_parse_int_option(argv[i] + 13, 1, INT_MAX, &cfg->work_iters) != 0) {
                die_invalid_option("--work-iters", argv[i] + 13);
            }
        } else if (!strncmp(argv[i], "--degraded-timeout-sec=", 23)) {
            if (tsd_parse_int_option(argv[i] + 23, 1, 86400, &cfg->degraded_timeout_sec) != 0) {
                die_invalid_option("--degraded-timeout-sec", argv[i] + 23);
            }
        } else if (!strncmp(argv[i], "--policy-slo-ratio=", 19)) {
            double ratio = 0.0;
            uint64_t scaled = 0;
            if (tsd_parse_ratio_option(argv[i] + 19, 0.5, 10.0, &ratio, &scaled) != 0) {
                die_invalid_option("--policy-slo-ratio", argv[i] + 19);
            }
            cfg->policy.slo_ratio_milli = (uint32_t)scaled;
        } else if (!strncmp(argv[i], "--policy-slo-temp=", 18)) {
            int temp_c = 0;
            if (tsd_parse_int_option(argv[i] + 18, -100, 200, &temp_c) != 0) {
                die_invalid_option("--policy-slo-temp", argv[i] + 18);
            }
            cfg->policy.slo_temp_millic = temp_c * 1000;
        } else if (!strncmp(argv[i], "--policy-penalty-up=", 20)) {
            int penalty = 0;
            if (tsd_parse_int_option(argv[i] + 20, 0, 1000000, &penalty) != 0) {
                die_invalid_option("--policy-penalty-up", argv[i] + 20);
            }
            cfg->policy.transition_penalty_up_milli = (uint32_t)penalty;
        } else if (!strncmp(argv[i], "--policy-penalty-down=", 22)) {
            int penalty = 0;
            if (tsd_parse_int_option(argv[i] + 22, 0, 1000000, &penalty) != 0) {
                die_invalid_option("--policy-penalty-down", argv[i] + 22);
            }
            cfg->policy.transition_penalty_down_milli = (uint32_t)penalty;
        } else if (!strncmp(argv[i], "--policy-forecast=", 19)) {
            int horizon = 0;
            if (tsd_parse_int_option(argv[i] + 19, 0, 1000, &horizon) != 0) {
                die_invalid_option("--policy-forecast", argv[i] + 19);
            }
            cfg->policy.forecast_horizon = (uint32_t)horizon;
        } else if (!strcmp(argv[i], "--health-check")) {
            cfg->health_check_mode = 1;
        } else if (!strncmp(argv[i], "--log-level=", 12)) {
            tsd_log_level_t parsed_level;
            if (tsd_log_level_from_string(argv[i] + 12, &parsed_level) != 0) {
                die_invalid_option("--log-level", argv[i] + 12);
            }
            cfg->log_level = parsed_level;
        } else if (!strcmp(argv[i], "--help")) {
            tsd_runtime_config_print_usage(argv[0]);
            exit(0);
        } else {
            tsd_log_error(LOG_COMPONENT, "Unknown option: %s", argv[i]);
            tsd_runtime_config_print_usage(argv[0]);
            exit(1);
        }
    }

    cfg->down_ratio_milli = (uint64_t)(cfg->down_ratio * 1000.0 + 0.5);
    if (cfg->down_ratio_milli == 0) {
        cfg->down_ratio_milli = 1;
    }

    tsd_policy_config_apply_bounds(&cfg->policy);

    if (tsd_runtime_config_refresh_ticks(cfg) != 0) {
        exit(1);
    }

    g_tsd_config = *cfg;
    tsd_log_set_level(cfg->log_level);
    return 0;
}
#endif
