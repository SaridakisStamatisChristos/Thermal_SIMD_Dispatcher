#include <thermal/simd/thermal_signals.h>

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <unistd.h>

#include <thermal/simd/thermal_trampoline.h>

#ifndef TSD_ENABLE_TESTS
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

#define TSD_SAFE_WRITE_LITERAL(fd, text) \
    tsd_safe_write_buf((fd), (text), sizeof(text) - 1U)

static void tsd_safe_write_uint(int fd, unsigned int value) {
    char buf[16];
    size_t pos = sizeof(buf);

    do {
        unsigned int digit = value % 10U;
        value /= 10U;
        buf[--pos] = (char)('0' + digit);
    } while (value != 0U && pos > 0U);

    tsd_safe_write_buf(fd, buf + pos, sizeof(buf) - pos);
}

static void tsd_safe_write_width(int fd, unsigned char width) {
    switch ((simd_width_t)width) {
        case SIMD_SSE41:
            TSD_SAFE_WRITE_LITERAL(fd, "SSE4.1");
            break;
        case SIMD_AVX2:
            TSD_SAFE_WRITE_LITERAL(fd, "AVX2");
            break;
        case SIMD_AVX512:
            TSD_SAFE_WRITE_LITERAL(fd, "AVX-512");
            break;
        default:
            TSD_SAFE_WRITE_LITERAL(fd, "unknown");
            break;
    }
}

static void crash_signal_handler(int sig) {
    TSD_SAFE_WRITE_LITERAL(STDERR_FILENO, "\n[thermal_simd] caught signal ");
    tsd_safe_write_uint(STDERR_FILENO, (unsigned int)sig);
    TSD_SAFE_WRITE_LITERAL(STDERR_FILENO, " while dispatching. last_selected=");

    unsigned char last_width = atomic_load_explicit(&g_tsd_last_patched_width, memory_order_relaxed);
    unsigned char attempt = atomic_load_explicit(&g_tsd_last_patch_attempt, memory_order_relaxed);
    unsigned char active = atomic_load_explicit(&g_tsd_current_width_byte, memory_order_relaxed);

    tsd_safe_write_width(STDERR_FILENO, last_width);
    TSD_SAFE_WRITE_LITERAL(STDERR_FILENO, " last_attempt=");
    tsd_safe_write_width(STDERR_FILENO, attempt);
    TSD_SAFE_WRITE_LITERAL(STDERR_FILENO, " active=");
    tsd_safe_write_width(STDERR_FILENO, active);
    TSD_SAFE_WRITE_LITERAL(STDERR_FILENO, "\n");

    _exit(128 + sig);
}
#endif

void tsd_install_patch_signal_handlers(void) {
#ifndef TSD_ENABLE_TESTS
    struct sigaction sa = {0};
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
}

#ifndef TSD_ENABLE_TESTS
#undef TSD_SAFE_WRITE_LITERAL
#endif
