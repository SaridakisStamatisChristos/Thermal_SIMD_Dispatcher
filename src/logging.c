#define _GNU_SOURCE
#include <thermal/simd/logging.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static _Atomic int g_tsd_log_level = TSD_LOG_LEVEL_INFO;
static pthread_mutex_t g_tsd_log_lock = PTHREAD_MUTEX_INITIALIZER;

static const char* level_name(tsd_log_level_t level) {
    switch (level) {
        case TSD_LOG_LEVEL_ERROR: return "ERROR";
        case TSD_LOG_LEVEL_WARN:  return "WARN";
        case TSD_LOG_LEVEL_INFO:  return "INFO";
        case TSD_LOG_LEVEL_DEBUG: return "DEBUG";
        default:                  return "UNKNOWN";
    }
}

void tsd_log_set_level(tsd_log_level_t level) {
    if (level < TSD_LOG_LEVEL_ERROR) {
        level = TSD_LOG_LEVEL_ERROR;
    } else if (level > TSD_LOG_LEVEL_DEBUG) {
        level = TSD_LOG_LEVEL_DEBUG;
    }
    atomic_store_explicit(&g_tsd_log_level, (int)level, memory_order_release);
}

tsd_log_level_t tsd_log_get_level(void) {
    int level = atomic_load_explicit(&g_tsd_log_level, memory_order_acquire);
    if (level < TSD_LOG_LEVEL_ERROR) {
        level = TSD_LOG_LEVEL_ERROR;
    } else if (level > TSD_LOG_LEVEL_DEBUG) {
        level = TSD_LOG_LEVEL_DEBUG;
    }
    return (tsd_log_level_t)level;
}

int tsd_log_should_log(tsd_log_level_t level) {
    int current = atomic_load_explicit(&g_tsd_log_level, memory_order_acquire);
    return (int)level <= current;
}

static void format_timestamp(char *buf, size_t buf_len, long *out_nsec) {
    if (!buf || buf_len == 0) {
        if (out_nsec) {
            *out_nsec = 0;
        }
        return;
    }
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        snprintf(buf, buf_len, "1970-01-01T00:00:00Z");
        if (out_nsec) {
            *out_nsec = 0;
        }
        return;
    }
    struct tm tm_info = {0};
    gmtime_r(&ts.tv_sec, &tm_info);
    if (strftime(buf, buf_len, "%Y-%m-%dT%H:%M:%S", &tm_info) == 0) {
        snprintf(buf, buf_len, "1970-01-01T00:00:00");
    }
    if (out_nsec) {
        *out_nsec = ts.tv_nsec;
    }
}

static void write_log_line(tsd_log_level_t level, const char *component, const char *message) {
    char timestamp[32];
    long nsec = 0;
    format_timestamp(timestamp, sizeof(timestamp), &nsec);
    const char *level_str = level_name(level);
    long tid = syscall(SYS_gettid);
    if (tid < 0) {
        tid = (long)pthread_self();
    }
    if (!component || component[0] == '\0') {
        component = "core";
    }
    FILE *stream = (level <= TSD_LOG_LEVEL_WARN) ? stderr : stdout;
    pthread_mutex_lock(&g_tsd_log_lock);
    fprintf(stream, "%s.%09ldZ [%s] (%ld) %s: %s\n", timestamp, nsec, level_str, tid, component, message ? message : "");
    fflush(stream);
    pthread_mutex_unlock(&g_tsd_log_lock);
}

void tsd_log(tsd_log_level_t level, const char *component, const char *fmt, ...) {
    if (!tsd_log_should_log(level)) {
        return;
    }
    char stack_buf[512];
    char *msg = stack_buf;
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt ? fmt : "", args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return;
    }
    if ((size_t)needed >= sizeof(stack_buf)) {
        size_t size = (size_t)needed + 1;
        char *heap_buf = (char*)malloc(size);
        if (heap_buf) {
            vsnprintf(heap_buf, size, fmt ? fmt : "", args_copy);
            msg = heap_buf;
        } else {
            snprintf(stack_buf, sizeof(stack_buf), "%s", "<log oom>");
            msg = stack_buf;
        }
    }
    va_end(args_copy);
    write_log_line(level, component, msg);
    if (msg != stack_buf && msg != NULL) {
        free(msg);
    }
}

int tsd_log_level_from_string(const char *level_str, tsd_log_level_t *out_level) {
    if (!level_str || !out_level) {
        return -1;
    }
    if (strcasecmp(level_str, "error") == 0) {
        *out_level = TSD_LOG_LEVEL_ERROR;
        return 0;
    }
    if (strcasecmp(level_str, "warn") == 0 || strcasecmp(level_str, "warning") == 0) {
        *out_level = TSD_LOG_LEVEL_WARN;
        return 0;
    }
    if (strcasecmp(level_str, "info") == 0) {
        *out_level = TSD_LOG_LEVEL_INFO;
        return 0;
    }
    if (strcasecmp(level_str, "debug") == 0) {
        *out_level = TSD_LOG_LEVEL_DEBUG;
        return 0;
    }
    return -1;
}

const char* tsd_log_level_to_string(tsd_log_level_t level) {
    return level_name(level);
}

const char* tsd_log_strerror(int errnum, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return "";
    }
    buf[0] = '\0';
#if defined(_GNU_SOURCE) && !defined(__APPLE__)
    char *msg = strerror_r(errnum, buf, buf_len);
    if (msg) {
        return msg;
    }
#else
    if (strerror_r(errnum, buf, buf_len) == 0) {
        return buf;
    }
#endif
    snprintf(buf, buf_len, "errno %d", errnum);
    return buf;
}
