#include <thermal/simd/thermal_config.h>

#include <config/runtime_flags.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/jsmn.h"

#include <thermal/simd/config_parser.h>

#define LOG_COMPONENT "config"

#define TSD_MIN_TEMP_C 20
#define TSD_MAX_TEMP_C 125
#define TSD_MIN_MARGIN_C 0
#define TSD_MAX_MARGIN_C 60
#define TSD_MIN_TELEMETRY_INTERVAL_MS 10
#define TSD_MAX_TELEMETRY_INTERVAL_MS 60000
#define TSD_MIN_TELEMETRY_SKEW_MS 0
#define TSD_MAX_TELEMETRY_SKEW_MS 60000

static void die_invalid_option(const char *option, const char *value);
static void die_config_error(const char *path, const char *message);

static const tsd_policy_config k_default_policy_config = {
    .slo_ratio_milli = 1500,
    .slo_temp_millic = 85000,
    .transition_penalty_up_milli = 750,
    .transition_penalty_down_milli = 1000,
    .forecast_horizon = 5,
};

#ifdef TSD_DEFAULT_COEFF_PATH
#define TSD_COEFF_PATH_DEFAULT TSD_DEFAULT_COEFF_PATH
#else
#define TSD_COEFF_PATH_DEFAULT "config/controller_coeffs.json"
#endif

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
    .run_forever = 0,
    .work_iters = 10000000,
    .cooldown_down_ticks = 0,
    .cooldown_up_ticks = 0,
    .min_dwell_ticks = 0,
    .thermal_temp_weight_milli = 0,
    .thermal_ratio_weight_milli = 0,
    .degraded_timeout_sec = 120,
    .degraded_policy_active = 0,
    .health_check_mode = 0,
    .metrics_enabled = 1,
    .metrics_port = 9464,
    .metrics_bind_host = "127.0.0.1",
    .metrics_tls_cert_path = "",
    .metrics_tls_key_path = "",
    .metrics_tls_ca_path = "",
    .metrics_tls_require_client_auth = 0,
    .metrics_basic_auth_user = "",
    .metrics_basic_auth_pass = "",
    .statsd_host = "",
    .statsd_port = 8125,
    .telemetry_interval_ms = 50,
    .telemetry_max_skew_ms = 150,
    .telemetry_ewma_alpha = 0.25,
    .telemetry_profile_path = "",
    .predictive_temp_ceiling_c = 92,
    .predictive_safety_margin_c = 4,
    .predictive_emergency_margin_c = 10,
    .predictive_alpha = 0.25,
    .predictive_coeff_path = TSD_COEFF_PATH_DEFAULT,
    .log_level = TSD_LOG_LEVEL_INFO,
    .policy = {0},
};

tsd_runtime_config g_tsd_config;

/* Runtime degraded state is an overlay. The base configuration is never
 * rewritten after startup, eliminating whole-struct restore races. */
static _Atomic int g_tsd_degraded_active = 0;

static int saturated_double_int(int value) {
    if (value <= 0) return value;
    return value > INT_MAX / 2 ? INT_MAX : value * 2;
}

void tsd_runtime_config_set_defaults(tsd_runtime_config *cfg) {
    if (!cfg) return;
    *cfg = k_default_config;
    cfg->policy = k_default_policy_config;
    tsd_policy_config_apply_bounds(&cfg->policy);
    cfg->degraded_policy_active = 0;
    atomic_store_explicit(&g_tsd_degraded_active, 0, memory_order_release);
}

int tsd_runtime_config_is_degraded(void) {
    return atomic_load_explicit(&g_tsd_degraded_active, memory_order_acquire) != 0;
}

int tsd_runtime_config_effective_down_count(const tsd_runtime_config *cfg) {
    if (!cfg) return 1;
    int value = cfg->down_count;
    if (tsd_runtime_config_is_degraded() && value > 1) value -= 1;
    return value > 0 ? value : 1;
}

int tsd_runtime_config_effective_up_count(const tsd_runtime_config *cfg) {
    if (!cfg) return 1;
    int value = cfg->up_count;
    if (tsd_runtime_config_is_degraded() && value < 10) {
        value += 2;
        if (value > 10) value = 10;
    }
    return value > 0 ? value : 1;
}

int tsd_runtime_config_effective_cooldown_down_ms(const tsd_runtime_config *cfg) {
    if (!cfg) return 0;
    int value = cfg->cooldown_down_ms;
    if (tsd_runtime_config_is_degraded()) {
        value = value < 2000 ? 2000 : saturated_double_int(value);
    }
    return value > 0 ? value : 0;
}

int tsd_runtime_config_effective_cooldown_up_ms(const tsd_runtime_config *cfg) {
    if (!cfg) return 0;
    int value = cfg->cooldown_up_ms;
    if (tsd_runtime_config_is_degraded()) {
        value = value < 4000 ? 4000 : saturated_double_int(value);
    }
    return value > 0 ? value : 0;
}

int tsd_runtime_config_effective_min_dwell_ms(const tsd_runtime_config *cfg) {
    if (!cfg) return 0;
    int value = cfg->min_dwell_ms;
    if (tsd_runtime_config_is_degraded() && value < 500) value = 500;
    return value > 0 ? value : 0;
}

uint64_t tsd_runtime_config_effective_down_ratio_milli(const tsd_runtime_config *cfg) {
    if (!cfg) return 1500;
    uint64_t value = cfg->down_ratio_milli ? cfg->down_ratio_milli : 1500;
    if (tsd_runtime_config_is_degraded() && value > 1300) value = 1300;
    return value;
}

static void log_policy_change(const tsd_runtime_config *cfg, const char *reason, const char *state) {
    if (!cfg) return;
    tsd_log_warn(LOG_COMPONENT,
                 "event=policy_state state=%s reason=%s down_count=%d up_count=%d cooldown_down_ms=%d cooldown_up_ms=%d min_dwell_ms=%d down_ratio_milli=%" PRIu64,
                 state ? state : "unknown",
                 reason ? reason : "unknown",
                 tsd_runtime_config_effective_down_count(cfg),
                 tsd_runtime_config_effective_up_count(cfg),
                 tsd_runtime_config_effective_cooldown_down_ms(cfg),
                 tsd_runtime_config_effective_cooldown_up_ms(cfg),
                 tsd_runtime_config_effective_min_dwell_ms(cfg),
                 tsd_runtime_config_effective_down_ratio_milli(cfg));
}

void tsd_runtime_config_enter_degraded_mode(tsd_runtime_config *cfg, const char *reason) {
    if (!cfg) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_tsd_degraded_active, &expected, 1,
                                                  memory_order_acq_rel, memory_order_acquire)) {
        return;
    }
    log_policy_change(cfg, reason, "degraded");
}

void tsd_runtime_config_exit_degraded_mode(tsd_runtime_config *cfg, const char *reason) {
    if (!cfg) return;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(&g_tsd_degraded_active, &expected, 0,
                                                  memory_order_acq_rel, memory_order_acquire)) {
        return;
    }
    log_policy_change(cfg, reason, "recovered");
}

static int copy_string_field(char *dest, size_t dest_size, const char *value) {
    if (!dest || dest_size == 0 || !value) return -1;
    size_t len = strlen(value);
    if (len >= dest_size) return -1;
    memcpy(dest, value, len);
    dest[len] = '\0';
    return 0;
}

static int json_token_equals(const char *json, const jsmntok_t *tok, const char *text) {
    if (!json || !tok || !text || tok->type != JSMN_STRING) return 0;
    size_t len = (size_t)(tok->end - tok->start);
    return strlen(text) == len && strncmp(json + tok->start, text, len) == 0;
}

static int json_skip(const jsmntok_t *tokens, int token_count, int index) {
    if (!tokens || index < 0 || index >= token_count) return index;
    jsmntype_t type = tokens[index].type;
    if (type == JSMN_PRIMITIVE || type == JSMN_STRING || type == JSMN_UNDEFINED) return index + 1;
    int cursor = index + 1;
    while (cursor < token_count && tokens[cursor].parent >= index) cursor = json_skip(tokens, token_count, cursor);
    return cursor;
}

static int json_token_to_string(const char *json, const jsmntok_t *tok, char *dest, size_t dest_size) {
    if (!json || !tok || !dest || dest_size == 0 || tok->type != JSMN_STRING) return -1;
    size_t len = (size_t)(tok->end - tok->start);
    if (len >= dest_size) return -1;
    memcpy(dest, json + tok->start, len);
    dest[len] = '\0';
    return 0;
}

static int json_token_to_int(const char *json, const jsmntok_t *tok, long min, long max, int *out) {
    if (!json || !tok || !out || tok->type != JSMN_PRIMITIVE) return -1;
    size_t len = (size_t)(tok->end - tok->start);
    if (len == 0 || len >= 64) return -1;
    char buffer[64];
    memcpy(buffer, json + tok->start, len);
    buffer[len] = '\0';
    errno = 0;
    char *end = NULL;
    long value = strtol(buffer, &end, 10);
    if (errno != 0 || end == buffer || *end != '\0' || value < min || value > max) return -1;
    *out = (int)value;
    return 0;
}

static int json_token_to_double(const char *json, const jsmntok_t *tok, double min, double max, double *out) {
    if (!json || !tok || !out || tok->type != JSMN_PRIMITIVE) return -1;
    size_t len = (size_t)(tok->end - tok->start);
    if (len == 0 || len >= 64) return -1;
    char buffer[64];
    memcpy(buffer, json + tok->start, len);
    buffer[len] = '\0';
    errno = 0;
    char *end = NULL;
    double value = strtod(buffer, &end);
    if (errno != 0 || end == buffer || *end != '\0' || isnan(value) || isinf(value) || value < min || value > max) return -1;
    *out = value;
    return 0;
}

static int json_token_to_bool(const char *json, const jsmntok_t *tok, int *out) {
    if (!json || !tok || !out || tok->type != JSMN_PRIMITIVE) return -1;
    size_t len = (size_t)(tok->end - tok->start);
    const char *ptr = json + tok->start;
    if (len == 4 && strncmp(ptr, "true", 4) == 0) { *out = 1; return 0; }
    if (len == 5 && strncmp(ptr, "false", 5) == 0) { *out = 0; return 0; }
    if (len == 1 && (ptr[0] == '0' || ptr[0] == '1')) { *out = ptr[0] == '1'; return 0; }
    return -1;
}

static int parse_metrics_tls_object(const char *json, const jsmntok_t *tokens, int token_count, int index,
                                    tsd_runtime_config *cfg) {
    if (!json || !tokens || !cfg) return -1;
    const jsmntok_t *obj = &tokens[index];
    if (obj->type != JSMN_OBJECT) return json_skip(tokens, token_count, index);
    int cursor = index + 1;
    int end = json_skip(tokens, token_count, index);
    while (cursor < end) {
        const jsmntok_t *key = &tokens[cursor];
        if (key->type != JSMN_STRING) return -1;
        cursor++;
        int value_index = cursor;
        int next = json_skip(tokens, token_count, value_index);
        if (json_token_equals(json, key, "certificate")) {
            if (json_token_to_string(json, &tokens[value_index], cfg->metrics_tls_cert_path,
                                     sizeof(cfg->metrics_tls_cert_path)) != 0) return -1;
        } else if (json_token_equals(json, key, "private_key")) {
            if (json_token_to_string(json, &tokens[value_index], cfg->metrics_tls_key_path,
                                     sizeof(cfg->metrics_tls_key_path)) != 0) return -1;
        } else if (json_token_equals(json, key, "client_ca")) {
            if (json_token_to_string(json, &tokens[value_index], cfg->metrics_tls_ca_path,
                                     sizeof(cfg->metrics_tls_ca_path)) != 0) return -1;
        } else if (json_token_equals(json, key, "require_client_auth")) {
            if (json_token_to_bool(json, &tokens[value_index], &cfg->metrics_tls_require_client_auth) != 0) return -1;
        }
        cursor = next;
    }
    return end;
}

static int parse_metrics_basic_auth_object(const char *json, const jsmntok_t *tokens, int token_count, int index,
                                           tsd_runtime_config *cfg) {
    if (!json || !tokens || !cfg) return -1;
    const jsmntok_t *obj = &tokens[index];
    if (obj->type != JSMN_OBJECT) return json_skip(tokens, token_count, index);
    int cursor = index + 1;
    int end = json_skip(tokens, token_count, index);
    while (cursor < end) {
        const jsmntok_t *key = &tokens[cursor];
        if (key->type != JSMN_STRING) return -1;
        cursor++;
        int value_index = cursor;
        int next = json_skip(tokens, token_count, value_index);
        if (json_token_equals(json, key, "username")) {
            if (json_token_to_string(json, &tokens[value_index], cfg->metrics_basic_auth_user,
                                     sizeof(cfg->metrics_basic_auth_user)) != 0) return -1;
        } else if (json_token_equals(json, key, "password")) {
            if (json_token_to_string(json, &tokens[value_index], cfg->metrics_basic_auth_pass,
                                     sizeof(cfg->metrics_basic_auth_pass)) != 0) return -1;
        }
        cursor = next;
    }
    return end;
}

static int parse_metrics_statsd_object(const char *json, const jsmntok_t *tokens, int token_count, int index,
                                       tsd_runtime_config *cfg) {
    if (!json || !tokens || !cfg) return -1;
    const jsmntok_t *obj = &tokens[index];
    if (obj->type != JSMN_OBJECT) return json_skip(tokens, token_count, index);
    int cursor = index + 1;
    int end = json_skip(tokens, token_count, index);
    while (cursor < end) {
        const jsmntok_t *key = &tokens[cursor];
        if (key->type != JSMN_STRING) return -1;
        cursor++;
        int value_index = cursor;
        int next = json_skip(tokens, token_count, value_index);
        if (json_token_equals(json, key, "host")) {
            if (json_token_to_string(json, &tokens[value_index], cfg->statsd_host, sizeof(cfg->statsd_host)) != 0) return -1;
        } else if (json_token_equals(json, key, "port")) {
            if (json_token_to_int(json, &tokens[value_index], 1, 65535, &cfg->statsd_port) != 0) return -1;
        }
        cursor = next;
    }
    return end;
}

static int parse_metrics_object(const char *json, const jsmntok_t *tokens, int token_count, int index,
                                tsd_runtime_config *cfg) {
    if (!json || !tokens || !cfg) return -1;
    const jsmntok_t *obj = &tokens[index];
    if (obj->type != JSMN_OBJECT) return json_skip(tokens, token_count, index);
    int cursor = index + 1;
    int end = json_skip(tokens, token_count, index);
    while (cursor < end) {
        const jsmntok_t *key = &tokens[cursor];
        if (key->type != JSMN_STRING) return -1;
        cursor++;
        int value_index = cursor;
        int next = json_skip(tokens, token_count, value_index);
        if (json_token_equals(json, key, "bind_address")) {
            if (json_token_to_string(json, &tokens[value_index], cfg->metrics_bind_host, sizeof(cfg->metrics_bind_host)) != 0) return -1;
        } else if (json_token_equals(json, key, "port")) {
            if (json_token_to_int(json, &tokens[value_index], 0, 65535, &cfg->metrics_port) != 0) return -1;
            cfg->metrics_enabled = cfg->metrics_port > 0;
        } else if (json_token_equals(json, key, "tls")) {
            int nested_end = parse_metrics_tls_object(json, tokens, token_count, value_index, cfg);
            if (nested_end < 0) return -1;
            cursor = nested_end;
            continue;
        } else if (json_token_equals(json, key, "basic_auth")) {
            int nested_end = parse_metrics_basic_auth_object(json, tokens, token_count, value_index, cfg);
            if (nested_end < 0) return -1;
            cursor = nested_end;
            continue;
        } else if (json_token_equals(json, key, "statsd")) {
            int nested_end = parse_metrics_statsd_object(json, tokens, token_count, value_index, cfg);
            if (nested_end < 0) return -1;
            cursor = nested_end;
            continue;
        }
        cursor = next;
    }
    return end;
}

static int parse_predictive_object(const char *json, const jsmntok_t *tokens, int token_count, int index,
                                   tsd_runtime_config *cfg) {
    if (!json || !tokens || !cfg) return -1;
    const jsmntok_t *obj = &tokens[index];
    if (obj->type != JSMN_OBJECT) return json_skip(tokens, token_count, index);
    int cursor = index + 1;
    int end = json_skip(tokens, token_count, index);
    while (cursor < end) {
        const jsmntok_t *key = &tokens[cursor];
        if (key->type != JSMN_STRING) return -1;
        cursor++;
        int value_index = cursor;
        int next = json_skip(tokens, token_count, value_index);
        if (json_token_equals(json, key, "coeff_path")) {
            if (json_token_to_string(json, &tokens[value_index], cfg->predictive_coeff_path,
                                     sizeof(cfg->predictive_coeff_path)) != 0) return -1;
        } else if (json_token_equals(json, key, "temp_ceiling_c")) {
            if (json_token_to_int(json, &tokens[value_index], TSD_MIN_TEMP_C, TSD_MAX_TEMP_C,
                                  &cfg->predictive_temp_ceiling_c) != 0) return -1;
        } else if (json_token_equals(json, key, "safety_margin_c")) {
            if (json_token_to_int(json, &tokens[value_index], TSD_MIN_MARGIN_C, TSD_MAX_MARGIN_C,
                                  &cfg->predictive_safety_margin_c) != 0) return -1;
        } else if (json_token_equals(json, key, "emergency_margin_c")) {
            if (json_token_to_int(json, &tokens[value_index], TSD_MIN_MARGIN_C, TSD_MAX_MARGIN_C,
                                  &cfg->predictive_emergency_margin_c) != 0) return -1;
        } else if (json_token_equals(json, key, "alpha")) {
            double alpha = 0.0;
            if (json_token_to_double(json, &tokens[value_index], 0.0, 1.0, &alpha) != 0) return -1;
            cfg->predictive_alpha = alpha;
        }
        cursor = next;
    }
    return end;
}

static int parse_telemetry_object(const char *json, const jsmntok_t *tokens, int token_count, int index,
                                  tsd_runtime_config *cfg) {
    if (!json || !tokens || !cfg) return -1;
    const jsmntok_t *obj = &tokens[index];
    if (obj->type != JSMN_OBJECT) return json_skip(tokens, token_count, index);
    int cursor = index + 1;
    int end = json_skip(tokens, token_count, index);
    while (cursor < end) {
        const jsmntok_t *key = &tokens[cursor];
        if (key->type != JSMN_STRING) return -1;
        cursor++;
        int value_index = cursor;
        int next = json_skip(tokens, token_count, value_index);
        if (json_token_equals(json, key, "profile")) {
            if (json_token_to_string(json, &tokens[value_index], cfg->telemetry_profile_path,
                                     sizeof(cfg->telemetry_profile_path)) != 0) return -1;
        } else if (json_token_equals(json, key, "interval_ms")) {
            if (json_token_to_int(json, &tokens[value_index], TSD_MIN_TELEMETRY_INTERVAL_MS,
                                  TSD_MAX_TELEMETRY_INTERVAL_MS, &cfg->telemetry_interval_ms) != 0) return -1;
        } else if (json_token_equals(json, key, "max_skew_ms")) {
            if (json_token_to_int(json, &tokens[value_index], TSD_MIN_TELEMETRY_SKEW_MS,
                                  TSD_MAX_TELEMETRY_SKEW_MS, &cfg->telemetry_max_skew_ms) != 0) return -1;
        } else if (json_token_equals(json, key, "ewma")) {
            double ewma = 0.0;
            if (json_token_to_double(json, &tokens[value_index], 0.0, 1.0, &ewma) != 0) return -1;
            cfg->telemetry_ewma_alpha = ewma;
        }
        cursor = next;
    }
    return end;
}

static int parse_config_file(tsd_runtime_config *cfg, const char *path) {
    if (!cfg || !path) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        tsd_log_error(LOG_COMPONENT, "Failed to open config file '%s': %s", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    char *buffer = (char *)malloc((size_t)length + 1);
    if (!buffer) { fclose(fp); return -1; }
    size_t read = fread(buffer, 1, (size_t)length, fp);
    fclose(fp);
    if (read != (size_t)length) { free(buffer); return -1; }
    buffer[length] = '\0';

    size_t token_capacity = 128;
    jsmntok_t *tokens = NULL;
    int parsed = JSMN_ERROR_NOMEM;
    jsmn_parser parser;
    while (1) {
        tokens = (jsmntok_t *)calloc(token_capacity, sizeof(jsmntok_t));
        if (!tokens) { free(buffer); return -1; }
        jsmn_init(&parser);
        parsed = jsmn_parse(&parser, buffer, (size_t)length, tokens, (unsigned int)token_capacity);
        if (parsed == JSMN_ERROR_NOMEM) {
            free(tokens);
            token_capacity *= 2;
            continue;
        }
        break;
    }
    if (parsed < 0 || parsed == 0 || tokens[0].type != JSMN_OBJECT) {
        free(tokens);
        free(buffer);
        tsd_log_error(LOG_COMPONENT, "Invalid JSON in config file '%s'", path);
        return -1;
    }

    int cursor = 1;
    for (int pair = 0; pair < tokens[0].size; ++pair) {
        const jsmntok_t *key = &tokens[cursor];
        if (key->type != JSMN_STRING) { free(tokens); free(buffer); return -1; }
        cursor++;
        int value_index = cursor;
        if (json_token_equals(buffer, key, "predictive")) {
            int next = parse_predictive_object(buffer, tokens, parsed, value_index, cfg);
            if (next < 0) { free(tokens); free(buffer); return -1; }
            cursor = next;
            continue;
        }
        if (json_token_equals(buffer, key, "telemetry")) {
            int next = parse_telemetry_object(buffer, tokens, parsed, value_index, cfg);
            if (next < 0) { free(tokens); free(buffer); return -1; }
            cursor = next;
            continue;
        }
        if (json_token_equals(buffer, key, "metrics")) {
            int next = parse_metrics_object(buffer, tokens, parsed, value_index, cfg);
            if (next < 0) { free(tokens); free(buffer); return -1; }
            cursor = next;
            continue;
        }
        cursor = json_skip(tokens, parsed, value_index);
    }

    free(tokens);
    free(buffer);
    return 0;
}

static int validate_runtime_config(tsd_runtime_config *cfg) {
    if (!cfg) return -1;
    if ((cfg->metrics_tls_cert_path[0] != '\0') != (cfg->metrics_tls_key_path[0] != '\0')) {
        tsd_log_error(LOG_COMPONENT, "Both --metrics-cert and --metrics-key must be provided together");
        return -1;
    }
    if (cfg->metrics_tls_require_client_auth && cfg->metrics_tls_ca_path[0] == '\0') {
        tsd_log_error(LOG_COMPONENT, "Client auth requires --metrics-ca to be set");
        return -1;
    }
    if ((cfg->metrics_basic_auth_user[0] != '\0') != (cfg->metrics_basic_auth_pass[0] != '\0')) {
        tsd_log_error(LOG_COMPONENT, "--metrics-basic-auth requires both username and password");
        return -1;
    }
    if (cfg->statsd_host[0] == '\0') cfg->statsd_port = 0;
    if (cfg->telemetry_interval_ms < TSD_MIN_TELEMETRY_INTERVAL_MS ||
        cfg->telemetry_interval_ms > TSD_MAX_TELEMETRY_INTERVAL_MS) return -1;
    if (cfg->telemetry_max_skew_ms < TSD_MIN_TELEMETRY_SKEW_MS ||
        cfg->telemetry_max_skew_ms > TSD_MAX_TELEMETRY_SKEW_MS) return -1;
    if (cfg->predictive_temp_ceiling_c < TSD_MIN_TEMP_C || cfg->predictive_temp_ceiling_c > TSD_MAX_TEMP_C) return -1;
    if (cfg->predictive_safety_margin_c < TSD_MIN_MARGIN_C || cfg->predictive_safety_margin_c > TSD_MAX_MARGIN_C) return -1;
    if (cfg->predictive_emergency_margin_c < TSD_MIN_MARGIN_C || cfg->predictive_emergency_margin_c > TSD_MAX_MARGIN_C) return -1;
    if (cfg->predictive_alpha < 0.0 || cfg->predictive_alpha > 1.0) return -1;
    return 0;
}

int tsd_runtime_config_refresh_ticks(tsd_runtime_config *cfg) {
    if (!cfg) { errno = EINVAL; return -1; }
    long long raw_ticks = 0;
    if (tsd_compute_ticks_from_ms(cfg->check_interval_us, cfg->cooldown_down_ms,
                                  &cfg->cooldown_down_ticks, &raw_ticks) != 0) return -1;
    if (tsd_compute_ticks_from_ms(cfg->check_interval_us, cfg->cooldown_up_ms,
                                  &cfg->cooldown_up_ticks, &raw_ticks) != 0) return -1;
    if (tsd_compute_ticks_from_ms(cfg->check_interval_us, cfg->min_dwell_ms,
                                  &cfg->min_dwell_ticks, &raw_ticks) != 0) return -1;
    return 0;
}

static void die_invalid_option(const char *option, const char *value) {
    tsd_log_error(LOG_COMPONENT, "Invalid value for %s: '%s'", option, value ? value : "");
    exit(1);
}

static void die_config_error(const char *path, const char *message) {
    tsd_log_error(LOG_COMPONENT, "Configuration error in %s: %s",
                  path ? path : "(unknown)", message ? message : "unknown");
    exit(1);
}

void tsd_runtime_config_print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Options:\n");
    printf("  --config=FILE          Load configuration overrides from JSON file\n");
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
    printf("  --duration-sec=S       Finite demo duration in wall-clock seconds (default: 10)\n");
    printf("  --run-forever          Run until SIGINT/SIGTERM instead of using --duration-sec\n");
    printf("  --work-iters=N         Inner workload batch size (default: 10000000)\n");
    printf("  --metrics-port=P       Prometheus metrics port (0 to disable, default: %d)\n", k_default_config.metrics_port);
    printf("  --metrics-bind=ADDR    Metrics bind address (default: %s)\n", k_default_config.metrics_bind_host);
    printf("  --metrics-cert=PATH    Enable TLS for metrics endpoint (requires --metrics-key)\n");
    printf("  --metrics-key=PATH     TLS private key for metrics endpoint\n");
    printf("  --metrics-ca=PATH      Optional client CA bundle for metrics TLS\n");
    printf("  --metrics-require-client-auth Enforce mutual TLS for metrics\n");
    printf("  --metrics-basic-auth=user:pass Protect metrics endpoint with basic auth\n");
    printf("  --statsd-host=HOST     Enable StatsD export to HOST (default: disabled)\n");
    printf("  --statsd-port=PORT     StatsD UDP port (default: %d)\n", k_default_config.statsd_port);
    printf("  --degraded-timeout-sec=S Fail closed if hardware counters missing for S seconds (default: %d)\n", k_default_config.degraded_timeout_sec);
    printf("  --temp-ceiling=C       Predictive controller temperature ceiling (default: %d)\n", k_default_config.predictive_temp_ceiling_c);
    printf("  --safety-margin=C      Predictive safety margin below ceiling (default: %d)\n", k_default_config.predictive_safety_margin_c);
    printf("  --emergency-margin=C   Predictive emergency margin (default: %d)\n", k_default_config.predictive_emergency_margin_c);
    printf("  --predictive-alpha=A   Predictive CPI EWMA alpha [0-1] (default: %.2f)\n", k_default_config.predictive_alpha);
    printf("  --coeff-path=PATH      Controller coefficients JSON (default: %s)\n", k_default_config.predictive_coeff_path);
    printf("  --telemetry-interval=MS Telemetry fusion interval (default: %d)\n", k_default_config.telemetry_interval_ms);
    printf("  --telemetry-max-skew=MS Telemetry freshness window (default: %d)\n", k_default_config.telemetry_max_skew_ms);
    printf("  --telemetry-ewma=A     Telemetry EWMA alpha [0-1] (default: %.2f)\n", k_default_config.telemetry_ewma_alpha);
    printf("  --telemetry-profile=PATH Telemetry profile manifest\n");
    printf("  --policy-slo-ratio=R   Predictive policy CPI target ratio (default: %.3f)\n", (double)k_default_policy_config.slo_ratio_milli / 1000.0);
    printf("  --policy-slo-temp=C    Predictive policy package temperature target in Celsius (default: %.1f)\n", (double)k_default_policy_config.slo_temp_millic / 1000.0);
    printf("  --policy-penalty-up=M  Predictive policy upgrade penalty in milli-cost (default: %u)\n", k_default_policy_config.transition_penalty_up_milli);
    printf("  --policy-penalty-down=M Predictive policy downgrade penalty in milli-cost (default: %u)\n", k_default_policy_config.transition_penalty_down_milli);
    printf("  --policy-forecast=N    Predictive policy forecast horizon in samples (default: %u)\n", k_default_policy_config.forecast_horizon);
    printf("  --health-check         Run diagnostics and exit with status\n");
    printf("  --sandbox-only         Execute startup sandbox and exit\n");
    printf("  --log-level=LEVEL      Log verbosity (error, warn, info, debug)\n");
    printf("  --help                 Show this help\n");
}

int tsd_runtime_config_parse_cli(tsd_runtime_config *cfg, int argc, char **argv) {
    if (!cfg) return -1;
    tsd_runtime_config_set_defaults(cfg);

    const char *env_level = getenv("TSD_LOG_LEVEL");
    if (env_level && env_level[0] != '\0') {
        tsd_log_level_t parsed_env_level;
        if (tsd_log_level_from_string(env_level, &parsed_env_level) == 0) cfg->log_level = parsed_env_level;
        else tsd_log_warn(LOG_COMPONENT, "Ignoring invalid TSD_LOG_LEVEL '%s' (expected error|warn|info|debug)", env_level);
    }

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--config=", 9)) {
            const char *path = argv[i] + 9;
            if (path[0] == '\0') die_invalid_option("--config", path);
            if (parse_config_file(cfg, path) != 0) die_config_error(path, "failed to load configuration");
        } else if (!strncmp(argv[i], "--interval=", 11)) {
            if (tsd_parse_ms_option(argv[i] + 11, 1, 10000, &cfg->check_interval_us) != 0) die_invalid_option("--interval", argv[i] + 11);
        } else if (!strncmp(argv[i], "--down-count=", 13)) {
            if (tsd_parse_int_option(argv[i] + 13, 1, 100, &cfg->down_count) != 0) die_invalid_option("--down-count", argv[i] + 13);
        } else if (!strncmp(argv[i], "--up-count=", 11)) {
            if (tsd_parse_int_option(argv[i] + 11, 1, 100, &cfg->up_count) != 0) die_invalid_option("--up-count", argv[i] + 11);
        } else if (!strncmp(argv[i], "--down-ratio=", 13)) {
            if (tsd_parse_ratio_option(argv[i] + 13, 1.0, 10.0, &cfg->down_ratio, &cfg->down_ratio_milli) != 0) die_invalid_option("--down-ratio", argv[i] + 13);
        } else if (!strncmp(argv[i], "--cooldown-down=", 16)) {
            if (tsd_parse_int_option(argv[i] + 16, 1, 3600000, &cfg->cooldown_down_ms) != 0) die_invalid_option("--cooldown-down", argv[i] + 16);
        } else if (!strncmp(argv[i], "--cooldown-up=", 14)) {
            if (tsd_parse_int_option(argv[i] + 14, 1, 3600000, &cfg->cooldown_up_ms) != 0) die_invalid_option("--cooldown-up", argv[i] + 14);
        } else if (!strncmp(argv[i], "--min-dwell=", 12)) {
            if (tsd_parse_int_option(argv[i] + 12, 1, 3600000, &cfg->min_dwell_ms) != 0) die_invalid_option("--min-dwell", argv[i] + 12);
        } else if (!strcmp(argv[i], "--no-avx512")) {
            cfg->allow_avx512 = 0;
        } else if (!strcmp(argv[i], "--allow-avx512")) {
            cfg->allow_avx512 = 1;
        } else if (!strncmp(argv[i], "--memory-guard-div=", 20)) {
            if (tsd_parse_int_option(argv[i] + 20, 1, 1000, &cfg->memory_guard_divisor) != 0) die_invalid_option("--memory-guard-div", argv[i] + 20);
        } else if (!strncmp(argv[i], "--memory-guard-offset=", 23)) {
            if (tsd_parse_int_option(argv[i] + 23, 0, 1000000, &cfg->memory_guard_offset_milli) != 0) die_invalid_option("--memory-guard-offset", argv[i] + 23);
        } else if (!strncmp(argv[i], "--thermal-temp-weight=", 22)) {
            if (tsd_parse_int_option(argv[i] + 22, 0, 100000, &cfg->thermal_temp_weight_milli) != 0) die_invalid_option("--thermal-temp-weight", argv[i] + 22);
        } else if (!strncmp(argv[i], "--thermal-ratio-weight=", 23)) {
            if (tsd_parse_int_option(argv[i] + 23, 0, 100000, &cfg->thermal_ratio_weight_milli) != 0) die_invalid_option("--thermal-ratio-weight", argv[i] + 23);
        } else if (!strncmp(argv[i], "--duration-sec=", 15)) {
            if (tsd_parse_int_option(argv[i] + 15, 1, 86400, &cfg->demo_duration_sec) != 0) die_invalid_option("--duration-sec", argv[i] + 15);
        } else if (!strcmp(argv[i], "--run-forever")) {
            cfg->run_forever = 1;
        } else if (!strncmp(argv[i], "--work-iters=", 13)) {
            if (tsd_parse_int_option(argv[i] + 13, 1, INT_MAX, &cfg->work_iters) != 0) die_invalid_option("--work-iters", argv[i] + 13);
        } else if (!strncmp(argv[i], "--metrics-port=", 15)) {
            if (tsd_parse_int_option(argv[i] + 15, 0, 65535, &cfg->metrics_port) != 0) die_invalid_option("--metrics-port", argv[i] + 15);
            cfg->metrics_enabled = cfg->metrics_port > 0;
        } else if (!strncmp(argv[i], "--metrics-bind=", 15)) {
            const char *value = argv[i] + 15;
            if (value[0] == '\0' || copy_string_field(cfg->metrics_bind_host, sizeof(cfg->metrics_bind_host), value) != 0) die_invalid_option("--metrics-bind", value);
        } else if (!strncmp(argv[i], "--metrics-cert=", 15)) {
            const char *value = argv[i] + 15;
            if (value[0] == '\0' || copy_string_field(cfg->metrics_tls_cert_path, sizeof(cfg->metrics_tls_cert_path), value) != 0) die_invalid_option("--metrics-cert", value);
        } else if (!strncmp(argv[i], "--metrics-key=", 14)) {
            const char *value = argv[i] + 14;
            if (value[0] == '\0' || copy_string_field(cfg->metrics_tls_key_path, sizeof(cfg->metrics_tls_key_path), value) != 0) die_invalid_option("--metrics-key", value);
        } else if (!strncmp(argv[i], "--metrics-ca=", 13)) {
            const char *value = argv[i] + 13;
            if (value[0] == '\0' || copy_string_field(cfg->metrics_tls_ca_path, sizeof(cfg->metrics_tls_ca_path), value) != 0) die_invalid_option("--metrics-ca", value);
        } else if (!strcmp(argv[i], "--metrics-require-client-auth")) {
            cfg->metrics_tls_require_client_auth = 1;
        } else if (!strncmp(argv[i], "--metrics-basic-auth=", 21)) {
            const char *value = argv[i] + 21;
            const char *sep = strchr(value, ':');
            if (!sep || sep == value || sep[1] == '\0') die_invalid_option("--metrics-basic-auth", value);
            size_t user_len = (size_t)(sep - value);
            size_t pass_len = strlen(sep + 1);
            if (user_len >= sizeof(cfg->metrics_basic_auth_user) || pass_len >= sizeof(cfg->metrics_basic_auth_pass)) die_invalid_option("--metrics-basic-auth", value);
            memcpy(cfg->metrics_basic_auth_user, value, user_len);
            cfg->metrics_basic_auth_user[user_len] = '\0';
            memcpy(cfg->metrics_basic_auth_pass, sep + 1, pass_len);
            cfg->metrics_basic_auth_pass[pass_len] = '\0';
        } else if (!strncmp(argv[i], "--statsd-host=", 14)) {
            const char *value = argv[i] + 14;
            if (value[0] == '\0' || copy_string_field(cfg->statsd_host, sizeof(cfg->statsd_host), value) != 0) die_invalid_option("--statsd-host", value);
        } else if (!strncmp(argv[i], "--statsd-port=", 14)) {
            if (tsd_parse_int_option(argv[i] + 14, 1, 65535, &cfg->statsd_port) != 0) die_invalid_option("--statsd-port", argv[i] + 14);
        } else if (!strncmp(argv[i], "--degraded-timeout-sec=", 23)) {
            if (tsd_parse_int_option(argv[i] + 23, 1, 86400, &cfg->degraded_timeout_sec) != 0) die_invalid_option("--degraded-timeout-sec", argv[i] + 23);
        } else if (!strncmp(argv[i], "--temp-ceiling=", 15)) {
            if (tsd_parse_int_option(argv[i] + 15, TSD_MIN_TEMP_C, TSD_MAX_TEMP_C, &cfg->predictive_temp_ceiling_c) != 0) die_invalid_option("--temp-ceiling", argv[i] + 15);
        } else if (!strncmp(argv[i], "--safety-margin=", 16)) {
            if (tsd_parse_int_option(argv[i] + 16, TSD_MIN_MARGIN_C, TSD_MAX_MARGIN_C, &cfg->predictive_safety_margin_c) != 0) die_invalid_option("--safety-margin", argv[i] + 16);
        } else if (!strncmp(argv[i], "--emergency-margin=", 19)) {
            if (tsd_parse_int_option(argv[i] + 19, TSD_MIN_MARGIN_C, TSD_MAX_MARGIN_C, &cfg->predictive_emergency_margin_c) != 0) die_invalid_option("--emergency-margin", argv[i] + 19);
        } else if (!strncmp(argv[i], "--predictive-alpha=", 19)) {
            double alpha = 0.0;
            if (tsd_parse_double_option(argv[i] + 19, 0.0, 1.0, &alpha) != 0) die_invalid_option("--predictive-alpha", argv[i] + 19);
            cfg->predictive_alpha = alpha;
        } else if (!strncmp(argv[i], "--telemetry-interval=", 21)) {
            if (tsd_parse_int_option(argv[i] + 21, TSD_MIN_TELEMETRY_INTERVAL_MS, TSD_MAX_TELEMETRY_INTERVAL_MS, &cfg->telemetry_interval_ms) != 0) die_invalid_option("--telemetry-interval", argv[i] + 21);
        } else if (!strncmp(argv[i], "--telemetry-max-skew=", 21)) {
            if (tsd_parse_int_option(argv[i] + 21, TSD_MIN_TELEMETRY_SKEW_MS, TSD_MAX_TELEMETRY_SKEW_MS, &cfg->telemetry_max_skew_ms) != 0) die_invalid_option("--telemetry-max-skew", argv[i] + 21);
        } else if (!strncmp(argv[i], "--telemetry-ewma=", 17)) {
            double ewma = 0.0;
            if (tsd_parse_double_option(argv[i] + 17, 0.0, 1.0, &ewma) != 0) die_invalid_option("--telemetry-ewma", argv[i] + 17);
            cfg->telemetry_ewma_alpha = ewma;
        } else if (!strncmp(argv[i], "--telemetry-profile=", 20)) {
            const char *value = argv[i] + 20;
            if (value[0] == '\0' || copy_string_field(cfg->telemetry_profile_path, sizeof(cfg->telemetry_profile_path), value) != 0) die_invalid_option("--telemetry-profile", value);
        } else if (!strncmp(argv[i], "--coeff-path=", 13)) {
            const char *value = argv[i] + 13;
            if (value[0] == '\0' || copy_string_field(cfg->predictive_coeff_path, sizeof(cfg->predictive_coeff_path), value) != 0) die_invalid_option("--coeff-path", value);
        } else if (!strncmp(argv[i], "--policy-slo-ratio=", 19)) {
            double ratio = 0.0;
            uint64_t scaled = 0;
            if (tsd_parse_ratio_option(argv[i] + 19, 0.5, 10.0, &ratio, &scaled) != 0) die_invalid_option("--policy-slo-ratio", argv[i] + 19);
            cfg->policy.slo_ratio_milli = (uint32_t)scaled;
        } else if (!strncmp(argv[i], "--policy-slo-temp=", 18)) {
            int temp_c = 0;
            if (tsd_parse_int_option(argv[i] + 18, -100, 200, &temp_c) != 0) die_invalid_option("--policy-slo-temp", argv[i] + 18);
            cfg->policy.slo_temp_millic = temp_c * 1000;
        } else if (!strncmp(argv[i], "--policy-penalty-up=", 20)) {
            int penalty = 0;
            if (tsd_parse_int_option(argv[i] + 20, 0, 1000000, &penalty) != 0) die_invalid_option("--policy-penalty-up", argv[i] + 20);
            cfg->policy.transition_penalty_up_milli = (uint32_t)penalty;
        } else if (!strncmp(argv[i], "--policy-penalty-down=", 22)) {
            int penalty = 0;
            if (tsd_parse_int_option(argv[i] + 22, 0, 1000000, &penalty) != 0) die_invalid_option("--policy-penalty-down", argv[i] + 22);
            cfg->policy.transition_penalty_down_milli = (uint32_t)penalty;
        } else if (!strncmp(argv[i], "--policy-forecast=", 19)) {
            int horizon = 0;
            if (tsd_parse_int_option(argv[i] + 19, 0, 1000, &horizon) != 0) die_invalid_option("--policy-forecast", argv[i] + 19);
            cfg->policy.forecast_horizon = (uint32_t)horizon;
        } else if (!strcmp(argv[i], "--health-check")) {
            cfg->health_check_mode = 1;
        } else if (!strcmp(argv[i], "--sandbox-only")) {
            tsd_runtime_flags_set_sandbox_only(1);
        } else if (!strncmp(argv[i], "--log-level=", 12)) {
            tsd_log_level_t parsed_level;
            if (tsd_log_level_from_string(argv[i] + 12, &parsed_level) != 0) die_invalid_option("--log-level", argv[i] + 12);
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

    if (cfg->metrics_port <= 0) {
        cfg->metrics_port = 0;
        cfg->metrics_enabled = 0;
    } else if (!cfg->metrics_enabled) {
        cfg->metrics_enabled = 1;
    }
    if (cfg->metrics_bind_host[0] == '\0') snprintf(cfg->metrics_bind_host, sizeof(cfg->metrics_bind_host), "%s", k_default_config.metrics_bind_host);
    if (validate_runtime_config(cfg) != 0) exit(1);

    cfg->down_ratio_milli = (uint64_t)(cfg->down_ratio * 1000.0 + 0.5);
    if (cfg->down_ratio_milli == 0) cfg->down_ratio_milli = 1;
    tsd_policy_config_apply_bounds(&cfg->policy);
    if (tsd_runtime_config_refresh_ticks(cfg) != 0) exit(1);

    g_tsd_config = *cfg;
    tsd_log_set_level(cfg->log_level);
    return 0;
}
