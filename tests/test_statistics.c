#include <thermal/simd/statistics.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void check_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "test failure: %s\n", message);
        exit(1);
    }
}

static void check_uint_eq(uint64_t actual, uint64_t expected, const char *message) {
    if (actual != expected) {
        fprintf(stderr, "test failure: %s (expected %" PRIu64 " got %" PRIu64 ")\n", message, expected, actual);
        exit(1);
    }
}

static void check_uint32_eq(uint32_t actual, uint32_t expected, const char *message) {
    if (actual != expected) {
        fprintf(stderr, "test failure: %s (expected %u got %u)\n", message, expected, actual);
        exit(1);
    }
}

static void test_update_ewma_trending_up(void) {
    uint64_t prev = 1000;
    uint64_t next = tsd_update_ewma(prev, 2000, 2);
    check_true(next > prev, "ewma trending up increases value");
    check_true(next < 2000, "ewma trending up does not overshoot");
    check_uint_eq(next, 1250, "ewma trending up step");
}

static void test_update_ewma_trending_down(void) {
    uint64_t prev = 2000;
    uint64_t next = tsd_update_ewma(prev, 1000, 3);
    check_true(next < prev, "ewma trending down reduces value");
    check_true(next > 1000, "ewma trending down respects floor");
    check_uint_eq(next, 1875, "ewma trending down step");
}

static void test_update_ewma_equal(void) {
    check_uint_eq(tsd_update_ewma(1000, 1000, 3), 1000, "ewma equal sample stable");
    check_uint_eq(tsd_update_ewma(0, 500, 3), 500, "ewma zero previous uses sample");
    check_uint_eq(tsd_update_ewma(1000, 2000, 0), 2000, "ewma zero shift uses sample");
}

static void test_trimmed_mean_basics(void) {
    uint32_t values[] = {1000, 2000, 3000, 4000};
    uint32_t mean = tsd_compute_trimmed_mean(values, 4);
    check_uint32_eq(mean, 2500, "trimmed mean removes extremes");
}

static void test_trimmed_mean_small_samples(void) {
    uint32_t pair[] = {1000, 2000};
    check_uint32_eq(tsd_compute_trimmed_mean(pair, 2), 1500, "trimmed mean avg pair");
    uint32_t single[] = {1234};
    check_uint32_eq(tsd_compute_trimmed_mean(single, 1), 1234, "trimmed mean single");
    check_uint32_eq(tsd_compute_trimmed_mean(NULL, 0), 0, "trimmed mean empty");
}

static void test_trimmed_mean_caps_history(void) {
    uint32_t values[TSD_RATIO_HISTORY + 4];
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        values[i] = (uint32_t)(i * 100);
    }
    uint32_t mean = tsd_compute_trimmed_mean(values, sizeof(values) / sizeof(values[0]));
    check_true(mean >= values[1], "trimmed mean excludes first element");
    check_true(mean <= values[TSD_RATIO_HISTORY - 2], "trimmed mean excludes last element");
}

static void test_trimmed_mean_outlier_resilience(void) {
    uint32_t values[] = {5000, 1003, 1001, 1000, 1002, 1005};
    uint32_t mean = tsd_compute_trimmed_mean(values, sizeof(values) / sizeof(values[0]));
    check_uint32_eq(mean, 1002, "trimmed mean suppresses single outlier");
}

static void test_ewma_convergence(void) {
    uint64_t ewma = 0;
    for (int i = 0; i < 10; ++i) {
        ewma = tsd_update_ewma(ewma, 2000, 2);
    }
    check_true(ewma >= 1990 && ewma <= 2000, "ewma approaches steady state");
}

int main(void) {
    test_update_ewma_trending_up();
    test_update_ewma_trending_down();
    test_update_ewma_equal();
    test_trimmed_mean_basics();
    test_trimmed_mean_small_samples();
    test_trimmed_mean_caps_history();
    test_trimmed_mean_outlier_resilience();
    test_ewma_convergence();
    return 0;
}
