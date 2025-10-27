#include <thermal/simd/config_parser.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

int tsd_parse_int_option(const char *value, long min, long max, int *out) {
    if (!value || !out) return -1;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return -1;
    }
    if (parsed < min || parsed > max) {
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

int tsd_parse_ms_option(const char *value, int min_ms, int max_ms, int *out_us) {
    int parsed_ms;
    if (tsd_parse_int_option(value, min_ms, max_ms, &parsed_ms) != 0) {
        return -1;
    }
    long converted = (long)parsed_ms * 1000L;
    if (converted > INT_MAX) {
        return -1;
    }
    if (!out_us) {
        return -1;
    }
    *out_us = (int)converted;
    return 0;
}

int tsd_parse_ratio_option(const char *value, double min, double max, double *ratio_out, uint64_t *scaled_out) {
    if (!value || !ratio_out || !scaled_out) return -1;
    errno = 0;
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0') {
        return -1;
    }
    if (isnan(parsed) || isinf(parsed)) {
        return -1;
    }
    if (parsed < min || parsed > max) {
        return -1;
    }
    double scaled = parsed * 1000.0;
    if (scaled < 0.0 || scaled > (double)UINT64_MAX) {
        return -1;
    }
    *ratio_out = parsed;
    *scaled_out = (uint64_t)(scaled + 0.5);
    if (*scaled_out == 0 && parsed > 0.0) {
        *scaled_out = 1;
    }
    return 0;
}

int tsd_compute_ticks_from_ms(int interval_us, int ms, int *out_ticks, long long *raw_ticks_out) {
    if (raw_ticks_out) {
        *raw_ticks_out = -1;
    }
    if (!out_ticks) {
        errno = EINVAL;
        return -1;
    }
    if (interval_us <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (ms < 0) {
        errno = EINVAL;
        return -1;
    }
    long long total_us = (long long)ms * 1000LL;
    long long interval = (long long)interval_us;
    long long ticks64 = (total_us + interval - 1) / interval;
    if (raw_ticks_out) {
        *raw_ticks_out = ticks64;
    }
    if (ticks64 <= 0 || ticks64 > INT_MAX) {
        errno = ERANGE;
        return -1;
    }
    *out_ticks = (int)ticks64;
    return 0;
}
