#include <thermal/simd/thermal_signals.h>

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <thermal/simd/thermal_trampoline.h>

static void tsd_safe_write_buf(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t written = write(fd, buf, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (written == 0) {
            break;
        }
        buf += (size_t)written;
        len -= (size_t)written;
    }
}

static void tsd_safe_write_str(int fd, const char *str) {
    if (!str) {
        return;
    }
    size_t len = strlen(str);
    tsd_safe_write_buf(fd, str, len);
}

static void tsd_safe_write_uint(int fd, unsigned int value) {
    char buf[32];
    int written = snprintf(buf, sizeof(buf), "%u", value);
    if (written < 0) {
        return;
    }
    size_t len = (size_t)written;
    if ((size_t)written >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    tsd_safe_write_buf(fd, buf, len);
}

static const char* width_name_from_byte(unsigned char width) {
    switch ((simd_width_t)width) {
        case SIMD_SSE41: return "SSE4.1";
        case SIMD_AVX2:  return "AVX2";
        case SIMD_AVX512:return "AVX-512";
        default:         return "unknown";
    }
}

static void crash_signal_handler(int sig) {
    tsd_safe_write_str(STDERR_FILENO, "\n[thermal_simd] caught signal ");
    tsd_safe_write_uint(STDERR_FILENO, (unsigned int)sig);
    tsd_safe_write_str(STDERR_FILENO, " while patching. last_patched=");
    unsigned char last_width = atomic_load_explicit(&g_tsd_last_patched_width, memory_order_relaxed);
    unsigned char attempt = atomic_load_explicit(&g_tsd_last_patch_attempt, memory_order_relaxed);
    unsigned char active = atomic_load_explicit(&g_tsd_current_width_byte, memory_order_relaxed);
    tsd_safe_write_str(STDERR_FILENO, width_name_from_byte(last_width));
    tsd_safe_write_str(STDERR_FILENO, " last_attempt=");
    tsd_safe_write_str(STDERR_FILENO, width_name_from_byte(attempt));
    tsd_safe_write_str(STDERR_FILENO, " active=");
    tsd_safe_write_str(STDERR_FILENO, width_name_from_byte(active));
    tsd_safe_write_str(STDERR_FILENO, "\n");
    _exit(128 + sig);
}

void tsd_install_patch_signal_handlers(void) {
#ifndef TSD_ENABLE_TESTS
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
}
