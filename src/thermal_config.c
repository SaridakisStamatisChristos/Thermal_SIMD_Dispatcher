#include <thermal/simd/thermal_config.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <thermal/simd/config_parser.h>

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
};

tsd_runtime_config g_tsd_config;

void tsd_runtime_config_set_defaults(tsd_runtime_config *cfg) {
    if (!cfg) {
        return;
    }
    *cfg = k_default_config;
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
            fprintf(stderr, "Invalid sampling interval while processing %s\n", "--cooldown-down");
        } else {
            fprintf(stderr, "Value for %s results in unsupported tick count (%lld)\n",
                    "--cooldown-down", raw_ticks);
        }
        return -1;
    }
    if (tsd_compute_ticks_from_ms(cfg->check_interval_us, cfg->cooldown_up_ms,
                                  &cfg->cooldown_up_ticks, &raw_ticks) != 0) {
        if (errno == EINVAL) {
            fprintf(stderr, "Invalid sampling interval while processing %s\n", "--cooldown-up");
        } else {
            fprintf(stderr, "Value for %s results in unsupported tick count (%lld)\n",
                    "--cooldown-up", raw_ticks);
        }
        return -1;
    }
    if (tsd_compute_ticks_from_ms(cfg->check_interval_us, cfg->min_dwell_ms,
                                  &cfg->min_dwell_ticks, &raw_ticks) != 0) {
        if (errno == EINVAL) {
            fprintf(stderr, "Invalid sampling interval while processing %s\n", "--min-dwell");
        } else {
            fprintf(stderr, "Value for %s results in unsupported tick count (%lld)\n",
                    "--min-dwell", raw_ticks);
        }
        return -1;
    }
    return 0;
}

#ifndef TSD_ENABLE_TESTS
static void die_invalid_option(const char *option, const char *value) {
    fprintf(stderr, "Invalid value for %s: '%s'\n", option, value ? value : "");
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
    printf("  --duration-sec=S       Demo duration (default: 10)\n");
    printf("  --work-iters=N         Inner work iterations per second (default: 10000000)\n");
    printf("  --help                 Show this help\n");
}

int tsd_runtime_config_parse_cli(tsd_runtime_config *cfg, int argc, char **argv) {
    if (!cfg) {
        return -1;
    }
    tsd_runtime_config_set_defaults(cfg);

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
        } else if (!strncmp(argv[i], "--duration-sec=", 15)) {
            if (tsd_parse_int_option(argv[i] + 15, 1, 86400, &cfg->demo_duration_sec) != 0) {
                die_invalid_option("--duration-sec", argv[i] + 15);
            }
        } else if (!strncmp(argv[i], "--work-iters=", 13)) {
            if (tsd_parse_int_option(argv[i] + 13, 1, INT_MAX, &cfg->work_iters) != 0) {
                die_invalid_option("--work-iters", argv[i] + 13);
            }
        } else if (!strcmp(argv[i], "--help")) {
            tsd_runtime_config_print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            tsd_runtime_config_print_usage(argv[0]);
            exit(1);
        }
    }

    cfg->down_ratio_milli = (uint64_t)(cfg->down_ratio * 1000.0 + 0.5);
    if (cfg->down_ratio_milli == 0) {
        cfg->down_ratio_milli = 1;
    }

    if (tsd_runtime_config_refresh_ticks(cfg) != 0) {
        exit(1);
    }

    g_tsd_config = *cfg;
    return 0;
}
#endif
