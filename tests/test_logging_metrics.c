#include <thermal/simd/logging.h>
#include <thermal/simd/metrics.h>
#include <thermal/simd/simd_width.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "test failure: %s\n", message);
        exit(1);
    }
}

static void check_nonneg(int value, const char *message) {
    if (value < 0) {
        fprintf(stderr, "test failure: %s (errno=%d)\n", message, errno);
        exit(1);
    }
}

static void check_zero(int rc, const char *message) {
    if (rc != 0) {
        fprintf(stderr, "test failure: %s (errno=%d)\n", message, errno);
        exit(1);
    }
}

static ssize_t read_into_buffer(int fd, char *buffer, size_t buffer_len) {
    ssize_t total = 0;
    while ((size_t)total < buffer_len - 1) {
        ssize_t n = read(fd, buffer + total, buffer_len - 1 - (size_t)total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    buffer[total >= 0 ? (size_t)total : 0] = '\0';
    return total;
}

typedef void (*capture_fn)(void *arg);

static void capture_stream(FILE *stream, int fd, capture_fn fn, void *arg, char *buffer, size_t buffer_len) {
    int pipefd[2];
    check_zero(pipe(pipefd), "pipe");
    fflush(stream);
    int saved_fd = dup(fd);
    check_nonneg(saved_fd, "dup");
    check_nonneg(dup2(pipefd[1], fd), "dup2 redirect");
    close(pipefd[1]);
    fn(arg);
    fflush(stream);
    check_zero(close(fd), "close stream fd");
    read_into_buffer(pipefd[0], buffer, buffer_len);
    close(pipefd[0]);
    check_nonneg(dup2(saved_fd, fd), "restore dup2");
    check_zero(close(saved_fd), "close saved fd");
}

static void log_message(void *arg) {
    const char *message = (const char *)arg;
    tsd_log_info("test_component", "%s", message);
}

static void log_warning(void *arg) {
    const char *message = (const char *)arg;
    tsd_log_warn("test_component", "%s", message);
}

static void verify_logging_outputs(void) {
    tsd_log_set_level(TSD_LOG_LEVEL_DEBUG);
    char buffer[1024];

    const char *info_message = "informational payload";
    capture_stream(stdout, STDOUT_FILENO, log_message, (void *)info_message, buffer, sizeof(buffer));
    check_true(strstr(buffer, "test_component") != NULL, "info log missing component");
    check_true(strstr(buffer, info_message) != NULL, "info log missing payload");

    const char *warn_message = "warning payload";
    capture_stream(stderr, STDERR_FILENO, log_warning, (void *)warn_message, buffer, sizeof(buffer));
    check_true(strstr(buffer, "test_component") != NULL, "warn log missing component");
    check_true(strstr(buffer, warn_message) != NULL, "warn log missing payload");

    char long_message[600];
    memset(long_message, 'A', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';
    capture_stream(stdout, STDOUT_FILENO, log_message, long_message, buffer, sizeof(buffer));
    check_true(strstr(buffer, long_message) != NULL, "long message not emitted");
}

static void verify_log_level_controls(void) {
    tsd_log_set_level(TSD_LOG_LEVEL_INFO);
    check_true(tsd_log_get_level() == TSD_LOG_LEVEL_INFO, "info level clamp");
    check_true(tsd_log_should_log(TSD_LOG_LEVEL_ERROR), "error should log");
    check_true(tsd_log_should_log(TSD_LOG_LEVEL_WARN), "warn should log");
    check_true(tsd_log_should_log(TSD_LOG_LEVEL_INFO), "info should log");
    check_true(!tsd_log_should_log(TSD_LOG_LEVEL_DEBUG), "debug should not log");

    tsd_log_set_level(TSD_LOG_LEVEL_ERROR);
    check_true(tsd_log_get_level() == TSD_LOG_LEVEL_ERROR, "error clamp lower bound");
    check_true(!tsd_log_should_log(TSD_LOG_LEVEL_INFO), "info suppressed at error");

    tsd_log_set_level((tsd_log_level_t)100);
    check_true(tsd_log_get_level() == TSD_LOG_LEVEL_DEBUG, "high level clamps to debug");
}

static void verify_log_level_conversion(void) {
    tsd_log_level_t level = TSD_LOG_LEVEL_ERROR;
    check_true(tsd_log_level_from_string("warn", &level) == 0, "parse warn");
    check_true(level == TSD_LOG_LEVEL_WARN, "warn level value");
    check_true(tsd_log_level_from_string("INFO", &level) == 0, "parse info");
    check_true(level == TSD_LOG_LEVEL_INFO, "info level value");
    check_true(tsd_log_level_from_string("debug", &level) == 0, "parse debug");
    check_true(level == TSD_LOG_LEVEL_DEBUG, "debug level value");
    check_true(tsd_log_level_from_string("error", &level) == 0, "parse error");
    check_true(level == TSD_LOG_LEVEL_ERROR, "error level value");
    check_true(tsd_log_level_from_string("warning", &level) == 0, "parse warning alias");
    check_true(level == TSD_LOG_LEVEL_WARN, "warning alias level value");
    check_true(tsd_log_level_from_string(NULL, &level) != 0, "null string rejected");
    check_true(tsd_log_level_from_string("invalid", &level) != 0, "invalid string rejected");
    check_true(tsd_log_level_from_string("info", NULL) != 0, "null output rejected");

    check_true(strcmp(tsd_log_level_to_string(TSD_LOG_LEVEL_ERROR), "ERROR") == 0, "level to string error");
    check_true(strcmp(tsd_log_level_to_string(TSD_LOG_LEVEL_WARN), "WARN") == 0, "level to string warn");
    check_true(strcmp(tsd_log_level_to_string(TSD_LOG_LEVEL_INFO), "INFO") == 0, "level to string info");
    check_true(strcmp(tsd_log_level_to_string(TSD_LOG_LEVEL_DEBUG), "DEBUG") == 0, "level to string debug");
}

static void verify_strerror_wrapper(void) {
    char buffer[64];
    const char *msg = tsd_log_strerror(ENOENT, buffer, sizeof(buffer));
    check_true(msg != NULL, "strerror returned null");
    check_true(strlen(msg) > 0, "strerror empty message");

    const char *empty = tsd_log_strerror(ENOENT, buffer, 0);
    check_true(empty != NULL, "strerror zero buffer null");
    check_true(strcmp(empty, "") == 0, "strerror zero buffer output");
}

static void verify_metrics_counters(void) {
    tsd_metrics_snapshot_t before;
    tsd_metrics_snapshot(&before);

    tsd_metrics_increment(TSD_METRIC_PERF_FALLBACKS);
    tsd_metrics_add(TSD_METRIC_PERF_FALLBACKS, 5);
    tsd_metrics_increment(TSD_METRIC_COUNT);
    tsd_metrics_add(TSD_METRIC_COUNT, 10);

    tsd_metrics_snapshot_t after;
    tsd_metrics_snapshot(&after);

    uint64_t perf_fallbacks_delta = after.counters[TSD_METRIC_PERF_FALLBACKS] -
                                    before.counters[TSD_METRIC_PERF_FALLBACKS];
    check_true(perf_fallbacks_delta == 6, "perf fallback counter delta");

    for (int i = 0; i < TSD_METRIC_COUNT; ++i) {
        const char *name = tsd_metrics_counter_name((tsd_metric_counter_t)i);
        check_true(name != NULL, "metric name null");
        check_true(strlen(name) > 0, "metric name empty");
    }
    check_true(strcmp(tsd_metrics_counter_name((tsd_metric_counter_t)100), "invalid") == 0,
               "invalid metric name fallback");

    tsd_metrics_snapshot_t mid;
    tsd_metrics_snapshot(&mid);
    tsd_metrics_record_width_transition(SIMD_SSE41, SIMD_AVX2, 0);
    tsd_metrics_record_width_transition(SIMD_AVX2, SIMD_AVX512, -1);
    tsd_metrics_snapshot_t end;
    tsd_metrics_snapshot(&end);

    uint64_t transitions_delta = end.counters[TSD_METRIC_PATCH_TRANSITIONS] -
                                 mid.counters[TSD_METRIC_PATCH_TRANSITIONS];
    uint64_t failures_delta = end.counters[TSD_METRIC_PATCH_FAILURES] -
                               mid.counters[TSD_METRIC_PATCH_FAILURES];
    check_true(transitions_delta == 1, "patch transitions delta");
    check_true(failures_delta == 1, "patch failures delta");
}

int main(void) {
    verify_logging_outputs();
    verify_log_level_controls();
    verify_log_level_conversion();
    verify_strerror_wrapper();
    verify_metrics_counters();
    return 0;
}
