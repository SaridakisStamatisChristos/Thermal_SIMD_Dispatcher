#include <thermal/simd/config_parser.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "test failure: %s\n", message);
        exit(1);
    }
}

static void check_int_eq(long long actual, long long expected, const char *message) {
    if (actual != expected) {
        fprintf(stderr, "test failure: %s (expected %lld got %lld)\n", message, expected, actual);
        exit(1);
    }
}

static void test_parse_int_option(void) {
    int value = 0;
    check_true(tsd_parse_int_option("42", 1, 100, &value) == 0, "parse_int_option valid 42");
    check_int_eq(value, 42, "parse_int_option stores parsed value");
    check_true(tsd_parse_int_option("-1", -5, 5, &value) == 0, "parse_int_option negative value");
    check_int_eq(value, -1, "parse_int_option handles negative");
    check_true(tsd_parse_int_option("", 0, 10, &value) != 0, "parse_int_option rejects empty");
    check_true(tsd_parse_int_option("abc", 0, 10, &value) != 0, "parse_int_option rejects alpha");
    check_true(tsd_parse_int_option("11", 0, 10, &value) != 0, "parse_int_option upper bound");
}

static void test_parse_ms_option(void) {
    int usec = 0;
    check_true(tsd_parse_ms_option("5", 1, 10, &usec) == 0, "parse_ms_option valid");
    check_int_eq(usec, 5000, "parse_ms_option conversion");
    check_true(tsd_parse_ms_option("1", 1, 10, &usec) == 0, "parse_ms_option lower bound");
    check_int_eq(usec, 1000, "parse_ms_option stores lower bound");
    check_true(tsd_parse_ms_option("0", 1, 10, &usec) != 0, "parse_ms_option rejects zero");
    char large[32];
    snprintf(large, sizeof(large), "%d", INT_MAX / 4000);
    check_true(tsd_parse_ms_option(large, 1, INT_MAX / 4000, &usec) == 0, "parse_ms_option large safe");
    check_true(tsd_parse_ms_option("10000000", 1, 10000000, &usec) != 0, "parse_ms_option overflow guard");
}

static void test_parse_ratio_option(void) {
    double ratio = 0.0;
    uint64_t scaled = 0;
    check_true(tsd_parse_ratio_option("1.5", 0.5, 2.0, &ratio, &scaled) == 0, "parse_ratio_option valid");
    check_true(ratio > 1.49 && ratio < 1.51, "parse_ratio_option ratio precision");
    check_int_eq((long long)scaled, 1500, "parse_ratio_option scaling");
    check_true(tsd_parse_ratio_option("0.4", 0.5, 2.0, &ratio, &scaled) != 0, "parse_ratio_option lower bound");
    check_true(tsd_parse_ratio_option("nan", 0.5, 2.0, &ratio, &scaled) != 0, "parse_ratio_option rejects nan");
    check_true(tsd_parse_ratio_option("10.0", 0.5, 10.0, &ratio, &scaled) == 0, "parse_ratio_option upper inclusive");
    check_int_eq((long long)scaled, 10000, "parse_ratio_option upper scaling");
}

static void test_parse_ratio_option_min_scaled(void) {
    double ratio = 0.0;
    uint64_t scaled = 0;
    check_true(tsd_parse_ratio_option("0.0005", 0.0005, 2.0, &ratio, &scaled) == 0,
               "parse_ratio_option accepts tiny positive value");
    check_true(ratio > 0.0, "parse_ratio_option tiny ratio recorded");
    check_int_eq((long long)scaled, 1, "parse_ratio_option minimum scaled clamped");
}

static void test_compute_ticks_from_ms(void) {
    int ticks = 0;
    long long raw = 0;
    check_true(tsd_compute_ticks_from_ms(50000, 1000, &ticks, &raw) == 0, "compute_ticks_from_ms valid");
    check_int_eq(ticks, 20, "compute_ticks_from_ms integer rounding");
    check_int_eq(raw, 20, "compute_ticks_from_ms raw value");

    errno = 0;
    check_true(tsd_compute_ticks_from_ms(0, 1000, &ticks, &raw) != 0, "compute_ticks_from_ms rejects zero interval");
    check_int_eq(errno, EINVAL, "compute_ticks_from_ms errno interval");

    errno = 0;
    check_true(tsd_compute_ticks_from_ms(50000, -1, &ticks, &raw) != 0, "compute_ticks_from_ms rejects negative ms");
    check_int_eq(errno, EINVAL, "compute_ticks_from_ms errno negative");

    errno = 0;
    check_true(tsd_compute_ticks_from_ms(1, INT_MAX, &ticks, &raw) != 0, "compute_ticks_from_ms overflow");
    check_int_eq(errno, ERANGE, "compute_ticks_from_ms errno overflow");
    check_true(raw > INT_MAX, "compute_ticks_from_ms raw overflow");
}

int main(void) {
    test_parse_int_option();
    test_parse_ms_option();
    test_parse_ratio_option();
    test_parse_ratio_option_min_scaled();
    test_compute_ticks_from_ms();
    return 0;
}
