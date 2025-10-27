#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <asm/unistd.h>
#include <cpuid.h>

#include "config_parser.h"
#include "statistics.h"
#include "thermal_simd_internal.h"
#ifdef TSD_ENABLE_TESTS
#include "thermal_simd_test.h"
#endif

// ============================================================================
// 0. RUNTIME CONFIGURATION (CLI-tunable)
// ============================================================================

static const int TSD_DEFAULT_CHECK_INTERVAL_US = 50000;
static const int TSD_DEFAULT_DOWN_COUNT = 3;
static const int TSD_DEFAULT_UP_COUNT = 5;
static const double TSD_DEFAULT_DOWN_RATIO = 1.5;
static const uint64_t TSD_DEFAULT_DOWN_RATIO_MILLI = 1500;
static const int TSD_DEFAULT_COOLDOWN_DOWN_MS = 1000;
static const int TSD_DEFAULT_COOLDOWN_UP_MS = 2000;
static const int TSD_DEFAULT_ALLOW_AVX512 = 0;
static const int TSD_DEFAULT_MIN_DWELL_MS = 200;
static const int TSD_DEFAULT_MEMORY_GUARD_DIVISOR = 5;
static const int TSD_DEFAULT_MEMORY_GUARD_OFFSET_MILLI = 200;
static const int TSD_DEFAULT_DEMO_DURATION_SEC = 10;
static const int TSD_DEFAULT_WORK_ITERS = 10000000;

static int param_check_interval_us = 50000;     // Check every 50ms
static int param_down_count = 3;                // Downgrade after 3 throttles
static int param_up_count = 5;                  // Upgrade after 5 stable
static double param_down_ratio = 1.5;           // Throttle threshold (1.5x baseline CPI)
static uint64_t param_down_ratio_milli = 1500;  // Scaled ratio (x1000) for integer math
static int param_cooldown_down_ms = 1000;       // 1s cooldown after downgrade
static int param_cooldown_up_ms = 2000;         // 2s cooldown after upgrade
static int param_allow_avx512 = 0;              // Allow AVX-512 (disable for Zen or conservative policy)
static int param_min_dwell_ms = 200;            // Minimum 200ms per width (prevents rapid flipping)
static int param_memory_guard_divisor = 5;      // Dynamic threshold divisor when cache bound
static int param_memory_guard_offset_milli = 200; // Additional guard (milli-ratio)

#define RATIO_HISTORY TSD_RATIO_HISTORY
#define FAST_EWMA_SHIFT 2
#define SLOW_EWMA_SHIFT 5
#define MPKI_SCALE 1000000ULL

// Demo controls
static int demo_duration_sec = TSD_DEFAULT_DEMO_DURATION_SEC;              // --duration-sec
static int work_iters = TSD_DEFAULT_WORK_ITERS;               // --work-iters

// Computed tick values (set after CLI parsing)
static int cooldown_down_ticks;
static int cooldown_up_ticks;
static int min_dwell_ticks;

static void
#ifdef TSD_ENABLE_TESTS
__attribute__((unused))
#endif
print_usage(const char *prog) {
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

static void
#ifdef TSD_ENABLE_TESTS
__attribute__((unused))
#endif
die_invalid_option(const char *option, const char *value) {
    fprintf(stderr, "Invalid value for %s: '%s'\n", option, value);
    exit(1);
}

static int refresh_tick_config(void) {
    long long raw_ticks = 0;
    if (tsd_compute_ticks_from_ms(param_check_interval_us, param_cooldown_down_ms, &cooldown_down_ticks, &raw_ticks) != 0) {
        if (errno == EINVAL) {
            fprintf(stderr, "Invalid sampling interval while processing %s\n", "--cooldown-down");
        } else {
            fprintf(stderr, "Value for %s results in unsupported tick count (%lld)\n", "--cooldown-down", raw_ticks);
        }
        return -1;
    }
    if (tsd_compute_ticks_from_ms(param_check_interval_us, param_cooldown_up_ms, &cooldown_up_ticks, &raw_ticks) != 0) {
        if (errno == EINVAL) {
            fprintf(stderr, "Invalid sampling interval while processing %s\n", "--cooldown-up");
        } else {
            fprintf(stderr, "Value for %s results in unsupported tick count (%lld)\n", "--cooldown-up", raw_ticks);
        }
        return -1;
    }
    if (tsd_compute_ticks_from_ms(param_check_interval_us, param_min_dwell_ms, &min_dwell_ticks, &raw_ticks) != 0) {
        if (errno == EINVAL) {
            fprintf(stderr, "Invalid sampling interval while processing %s\n", "--min-dwell");
        } else {
            fprintf(stderr, "Value for %s results in unsupported tick count (%lld)\n", "--min-dwell", raw_ticks);
        }
        return -1;
    }
    return 0;
}

#ifndef TSD_ENABLE_TESTS
static void parse_flags(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--interval=", 11)) {
            if (tsd_parse_ms_option(argv[i] + 11, 1, 10000, &param_check_interval_us) != 0) {
                die_invalid_option("--interval", argv[i] + 11);
            }
        } else if (!strncmp(argv[i], "--down-count=", 13)) {
            if (tsd_parse_int_option(argv[i] + 13, 1, 100, &param_down_count) != 0) {
                die_invalid_option("--down-count", argv[i] + 13);
            }
        } else if (!strncmp(argv[i], "--up-count=", 11)) {
            if (tsd_parse_int_option(argv[i] + 11, 1, 100, &param_up_count) != 0) {
                die_invalid_option("--up-count", argv[i] + 11);
            }
        } else if (!strncmp(argv[i], "--down-ratio=", 13)) {
            if (tsd_parse_ratio_option(argv[i] + 13, 1.0, 10.0, &param_down_ratio, &param_down_ratio_milli) != 0) {
                die_invalid_option("--down-ratio", argv[i] + 13);
            }
        } else if (!strncmp(argv[i], "--cooldown-down=", 16)) {
            if (tsd_parse_int_option(argv[i] + 16, 1, 3600000, &param_cooldown_down_ms) != 0) {
                die_invalid_option("--cooldown-down", argv[i] + 16);
            }
        } else if (!strncmp(argv[i], "--cooldown-up=", 14)) {
            if (tsd_parse_int_option(argv[i] + 14, 1, 3600000, &param_cooldown_up_ms) != 0) {
                die_invalid_option("--cooldown-up", argv[i] + 14);
            }
        } else if (!strncmp(argv[i], "--min-dwell=", 12)) {
            if (tsd_parse_int_option(argv[i] + 12, 1, 3600000, &param_min_dwell_ms) != 0) {
                die_invalid_option("--min-dwell", argv[i] + 12);
            }
        } else if (!strcmp(argv[i], "--no-avx512")) {
            param_allow_avx512 = 0;
        } else if (!strcmp(argv[i], "--allow-avx512")) {
            param_allow_avx512 = 1;
        } else if (!strncmp(argv[i], "--memory-guard-div=", 20)) {
            if (tsd_parse_int_option(argv[i] + 20, 1, 1000, &param_memory_guard_divisor) != 0) {
                die_invalid_option("--memory-guard-div", argv[i] + 20);
            }
        } else if (!strncmp(argv[i], "--memory-guard-offset=", 23)) {
            if (tsd_parse_int_option(argv[i] + 23, 0, 1000000, &param_memory_guard_offset_milli) != 0) {
                die_invalid_option("--memory-guard-offset", argv[i] + 23);
            }
        } else if (!strncmp(argv[i], "--duration-sec=", 15)) {
            if (tsd_parse_int_option(argv[i] + 15, 1, 86400, &demo_duration_sec) != 0) {
                die_invalid_option("--duration-sec", argv[i] + 15);
            }
        } else if (!strncmp(argv[i], "--work-iters=", 13)) {
            if (tsd_parse_int_option(argv[i] + 13, 1, INT_MAX, &work_iters) != 0) {
                die_invalid_option("--work-iters", argv[i] + 13);
            }
        } else if (!strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        }
    }

    param_down_ratio_milli = (uint64_t)(param_down_ratio * 1000.0 + 0.5);
    if (param_down_ratio_milli == 0) {
        param_down_ratio_milli = 1;
    }

    if (refresh_tick_config() != 0) {
        exit(1);
    }
}
#endif

// ============================================================================
// 1. CORRECT FEATURE DETECTION (with AVX bit check)
// ============================================================================

static int cpu_has_sse41(void) {
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;
    return (ecx & (1u << 19)) != 0; // CPUID.1:ECX[19] = SSE4.1
}

static int cpu_has_avx_bit(void) {
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;
    return (ecx & (1u << 28)) != 0; // CPUID.1:ECX[28] = AVX
}

uint8_t g_avx_available = 0; // Global for shim AVX transition guard

static int check_xsave_support(void) {
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;
    if (!(ecx & (1 << 27)))
        return 0;
    uint64_t xcr0;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    xcr0 = ((uint64_t)edx << 32) | eax;
    return (xcr0 & 0x6) == 0x6; // bits 1 & 2
}

static simd_width_t detect_max_simd(void) {
#ifdef TSD_ENABLE_TESTS
    extern simd_width_t (*tsd_test_detect_hook)(void);
    if (tsd_test_detect_hook) {
        return tsd_test_detect_hook();
    }
#endif
    unsigned int eax, ebx, ecx, edx;
    if (!cpu_has_sse41()) {
        return SIMD_SSE41; // Will error out in main
    }
    int xsave_ok = check_xsave_support();
    if (!xsave_ok) {
        return SIMD_SSE41;
    }
    if (cpu_has_avx_bit() && xsave_ok) {
        g_avx_available = 1;
    }
    if (param_allow_avx512 && __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if ((ebx & (1 << 16)) && cpu_has_avx_bit()) {
            uint64_t xcr0;
            __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
            xcr0 = ((uint64_t)edx << 32) | eax;
            if ((xcr0 & 0xE6) == 0xE6)
                return SIMD_AVX512;
        }
        if ((ebx & (1 << 5)) && cpu_has_avx_bit())
            return SIMD_AVX2;
    } else if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if ((ebx & (1 << 5)) && cpu_has_avx_bit())
            return SIMD_AVX2;
    }
    return SIMD_SSE41;
}

// ============================================================================
// 2. DOUBLE-BUFFERED TRAMPOLINE (strict W^X, no execution gap)
// ============================================================================

typedef struct {
    uint8_t code[16];
} patch_slot_t __attribute__((aligned(16)));

typedef struct {
    patch_slot_t *active;
    patch_slot_t *inactive;
    void *page_a;
    void *page_b;
} trampoline_ctx_t;

static trampoline_ctx_t trampoline_ctx;
static pthread_mutex_t patch_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(simd_width_t) current_width;  // Atomic to avoid data race in shim
static _Atomic unsigned char current_width_byte;  // Mirror for shim byte-read (C11-safe)
static _Atomic int trampoline_initialized;  // Tracks whether trampoline was patched at least once
static _Atomic(patch_slot_t*) active_trampoline __attribute__((aligned(16)));
static _Atomic unsigned char last_patch_attempt;  // For diagnostics
static _Atomic unsigned char last_patched_width;  // For diagnostics
static int warned_llc_unavailable = 0;
static int warned_perf_group_layout = 0;
static _Atomic uint64_t workload_iterations = 0;

#ifdef TSD_ENABLE_TESTS
simd_width_t (*tsd_test_detect_hook)(void) = NULL;
static const uint8_t *test_patch_override[3] = { NULL, NULL, NULL };
static size_t test_patch_override_size[3] = { 0, 0, 0 };
static char test_last_patch_error[256] = {0};
static tsd_patch_fail_stage_t test_patch_fail_stage = TSD_PATCH_FAIL_NONE;
typedef struct {
    uint32_t ratios[128];
    size_t count;
    size_t index;
    uint32_t mpki;
    int enabled;
} test_perf_script_t;
static test_perf_script_t test_perf_script = {0};
#endif

// CORRECTED ENCODINGS (verified with objdump)
// XMM-only variants (128-bit, scalar-exact, minimal power/downclock)
static const uint8_t PATCH_SSE41[16]  = { 0xF3,0x0F,0x1E,0xFA, 0x66,0x0F,0x38,0x40,0xC1, 0xC3, 0x90,0x90,0x90,0x90,0x90,0x90 }; // ENDBR64; pmulld xmm0,xmm1; ret; nop padding
static const uint8_t PATCH_AVX2[16]   = { 0xF3,0x0F,0x1E,0xFA, 0xC4,0xE2,0x79,0x40,0xC1, 0xC3, 0x90,0x90,0x90,0x90,0x90,0x90 }; // ENDBR64; vpmulld xmm0,xmm0,xmm1; ret; nop padding
static const uint8_t PATCH_AVX512[16] = { 0xF3,0x0F,0x1E,0xFA, 0x62,0xF2,0x79,0x08,0x40,0xC1, 0xC3, 0x90,0x90,0x90,0x90,0x90 }; // ENDBR64; vpmulld xmm0{k0},xmm0,xmm1; ret; nop padding

_Static_assert(sizeof(PATCH_SSE41) == 16, "SSE41 payload must be 16 bytes");
_Static_assert(sizeof(PATCH_AVX2) == 16, "AVX2 payload must be 16 bytes");
_Static_assert(sizeof(PATCH_AVX512) == 16, "AVX512 payload must be 16 bytes");

static void* create_rx_page(void) {
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        return NULL;
    }
    size_t pagesize = (size_t)pagesize_long;
    void *mem = mmap(NULL, pagesize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    return mem;
}

static void* page_align(void *ptr, size_t pagesize) {
    if (!ptr) return NULL;
    uintptr_t addr = (uintptr_t)ptr;
    if (pagesize == 0) return NULL;
    uintptr_t remainder = addr % pagesize;
    return (void*)(addr - remainder);
}

static void safe_write_buf(int fd, const char *buf, size_t len) {
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
        buf += written;
        len -= (size_t)written;
    }
}

static void safe_write_str(int fd, const char *str) {
    if (!str) return;
    size_t len = 0;
    while (str[len] != '\0') len++;
    safe_write_buf(fd, str, len);
}

static void safe_write_uint(int fd, unsigned int value) {
    char buf[32];
    size_t idx = sizeof(buf);
    if (value == 0) {
        buf[--idx] = '0';
    } else {
        while (value > 0 && idx > 0) {
            buf[--idx] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    safe_write_buf(fd, buf + idx, sizeof(buf) - idx);
}

static const char* width_name_from_byte(unsigned char width) {
    switch ((simd_width_t)width) {
        case SIMD_SSE41: return "SSE4.1";
        case SIMD_AVX2:  return "AVX2";
        case SIMD_AVX512:return "AVX-512";
        default:         return "unknown";
    }
}

static void log_errno_message(const char *prefix, int err) {
    if (!prefix) {
        return;
    }
    fprintf(stderr, "[thermal_simd] %s: %s\n", prefix, strerror(err));
}

static void report_patch_error(const char *context, int err) {
    log_errno_message(context, err);
#ifdef TSD_ENABLE_TESTS
    if (context) {
        snprintf(test_last_patch_error, sizeof(test_last_patch_error), "%s: %s", context, strerror(err));
    }
#endif
}

static void
#ifdef TSD_ENABLE_TESTS
__attribute__((unused))
#endif
try_perf_ioctl(int fd, unsigned long request, const char *what) {
    if (fd < 0) {
        return;
    }
    if (ioctl(fd, request, 0) != 0) {
        log_errno_message(what, errno);
    }
}

static int should_fallback_to_software(int err) {
    return err == EACCES || err == EPERM || err == ENOENT || err == EOPNOTSUPP;
}

static uint64_t timespec_diff_ns(const struct timespec *start, const struct timespec *end) {
    if (!start || !end) {
        return 0;
    }
    time_t sec = end->tv_sec - start->tv_sec;
    long nsec = end->tv_nsec - start->tv_nsec;
    if (nsec < 0) {
        sec -= 1;
        nsec += 1000000000L;
    }
    if (sec < 0) {
        return 0;
    }
    return (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
}

static void
#ifdef TSD_ENABLE_TESTS
__attribute__((unused))
#endif
crash_signal_handler(int sig) {
    safe_write_str(STDERR_FILENO, "\n[thermal_simd] caught signal ");
    safe_write_uint(STDERR_FILENO, (unsigned int)sig);
    safe_write_str(STDERR_FILENO, " while patching. last_patched=");
    unsigned char last_width = __atomic_load_n(&last_patched_width, __ATOMIC_RELAXED);
    unsigned char attempt = __atomic_load_n(&last_patch_attempt, __ATOMIC_RELAXED);
    unsigned char active = __atomic_load_n(&current_width_byte, __ATOMIC_RELAXED);
    safe_write_str(STDERR_FILENO, width_name_from_byte(last_width));
    safe_write_str(STDERR_FILENO, " last_attempt=");
    safe_write_str(STDERR_FILENO, width_name_from_byte(attempt));
    safe_write_str(STDERR_FILENO, " active=");
    safe_write_str(STDERR_FILENO, width_name_from_byte(active));
    safe_write_str(STDERR_FILENO, "\n");
    _exit(128 + sig);
}

static void
#ifdef TSD_ENABLE_TESTS
__attribute__((unused))
#endif
install_signal_handlers(void)
#ifndef TSD_ENABLE_TESTS
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
#else
{}
#endif

int init_double_buffer_trampoline(void) {
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        log_errno_message("failed to query page size", errno ? errno : EINVAL);
        return -1;
    }
    size_t pagesize = (size_t)pagesize_long;
    trampoline_ctx.page_a = create_rx_page();
    trampoline_ctx.page_b = create_rx_page();
    if (!trampoline_ctx.page_a || !trampoline_ctx.page_b) {
        log_errno_message("failed to allocate RX trampoline pages", errno ? errno : ENOMEM);
        return -1;
    }
    trampoline_ctx.active = (patch_slot_t*)trampoline_ctx.page_a;
    trampoline_ctx.inactive = (patch_slot_t*)trampoline_ctx.page_b;
    if (mprotect(trampoline_ctx.page_a, pagesize, PROT_READ | PROT_EXEC) != 0 ||
        mprotect(trampoline_ctx.page_b, pagesize, PROT_READ | PROT_EXEC) != 0) {
        report_patch_error("mprotect(initial RX)", errno);
        return -1;
    }
    __atomic_store_n(&active_trampoline, trampoline_ctx.active, __ATOMIC_SEQ_CST);
    return 0;
}

static void serialize_instruction_stream(void) {
    __asm__ __volatile__(
        "mfence\n\t"
        "lfence\n\t"
        "cpuid\n\t"
        ::: "rax", "rbx", "rcx", "rdx", "memory"
    );
}

static void atomic_patch_strict_wx(simd_width_t new_width) {
    pthread_mutex_lock(&patch_lock);
    simd_width_t width = __atomic_load_n(&current_width, __ATOMIC_ACQUIRE);
    int initialized = __atomic_load_n(&trampoline_initialized, __ATOMIC_ACQUIRE);
    if (initialized && new_width == width) { pthread_mutex_unlock(&patch_lock); return; }
    const uint8_t *patch_data = NULL;
    size_t patch_size = 0;
    switch (new_width) {
        case SIMD_SSE41:
            patch_data = PATCH_SSE41;
            patch_size = sizeof(PATCH_SSE41);
            break;
        case SIMD_AVX2:
            patch_data = PATCH_AVX2;
            patch_size = sizeof(PATCH_AVX2);
            break;
        case SIMD_AVX512:
            patch_data = PATCH_AVX512;
            patch_size = sizeof(PATCH_AVX512);
            break;
        default:
            goto unlock;
    }
#ifdef TSD_ENABLE_TESTS
    if (test_patch_override[new_width]) {
        patch_data = test_patch_override[new_width];
        patch_size = test_patch_override_size[new_width];
    }
#endif
    __atomic_store_n(&last_patch_attempt, (unsigned char)new_width, __ATOMIC_RELAXED);
    if (patch_size == 0 || (patch_size % sizeof(uint64_t)) != 0) {
        goto unlock;
    }
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        goto unlock;
    }
    size_t pagesize = (size_t)pagesize_long;
    patch_slot_t *inactive = trampoline_ctx.inactive;
    if (!inactive) goto unlock;
    void *inactive_page = page_align(inactive, pagesize);
    if (!inactive_page) goto unlock;
    if (patch_size == 0) { goto unlock; }
#ifdef TSD_ENABLE_TESTS
    if (test_patch_fail_stage == TSD_PATCH_FAIL_PROTECT_WRITE) {
        errno = EPERM;
        report_patch_error("mprotect(trampoline write)", errno);
        test_patch_fail_stage = TSD_PATCH_FAIL_NONE;
        goto unlock;
    }
#endif
    if (mprotect(inactive_page, pagesize, PROT_READ | PROT_WRITE) != 0) {
        report_patch_error("mprotect(trampoline write)", errno);
        goto unlock;
    }
    for (size_t offset = 0; offset < patch_size; offset += sizeof(uint64_t)) {
        uint64_t chunk = 0;
        memcpy(&chunk, patch_data + offset, sizeof(uint64_t));
        __atomic_store_n((uint64_t*)(inactive->code + offset), chunk, __ATOMIC_SEQ_CST);
    }
    serialize_instruction_stream();
    if (patch_size == 0) { goto unlock; }
#ifdef TSD_ENABLE_TESTS
    if (test_patch_fail_stage == TSD_PATCH_FAIL_PROTECT_EXEC) {
        errno = EPERM;
        report_patch_error("mprotect(trampoline exec)", errno);
        test_patch_fail_stage = TSD_PATCH_FAIL_NONE;
        goto unlock;
    }
#endif
    if (mprotect(inactive_page, pagesize, PROT_READ | PROT_EXEC) != 0) {
        report_patch_error("mprotect(trampoline exec)", errno);
        goto unlock;
    }
    __atomic_store_n(&current_width, new_width, __ATOMIC_RELEASE);
    __atomic_store_n(&current_width_byte, (unsigned char)new_width, __ATOMIC_RELEASE);
    __atomic_store_n(&active_trampoline, inactive, __ATOMIC_SEQ_CST);
    patch_slot_t *tmp = trampoline_ctx.active;
    trampoline_ctx.active = trampoline_ctx.inactive;
    trampoline_ctx.inactive = tmp;
    __atomic_store_n(&trampoline_initialized, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&last_patched_width, (unsigned char)new_width, __ATOMIC_RELEASE);
    printf("Patched to %s (strict W^X)\\n", new_width == SIMD_AVX512 ? "AVX-512" : new_width == SIMD_AVX2 ? "AVX2" : "SSE4.1");
unlock:
    pthread_mutex_unlock(&patch_lock);
}

// ============================================================================
// 3. SHIM LAYER (correct ABI + AVX transition guard)
// ============================================================================

__attribute__((naked))
static int32_t simd_shim(int32_t a __attribute__((unused)),
                         int32_t b __attribute__((unused))) {
    __asm__ __volatile__(
        "cmpb $0, g_avx_available(%rip)\n\t"
        "je 1f\n\t"
        "cmpb $0, current_width_byte(%rip)\n\t"
        "jne 1f\n\t"
        ".byte 0xC5, 0xF8, 0x77\n\t"  // vzeroupper
        "1:\n\t"
        "movd %edi, %xmm0\n\t"
        "movd %esi, %xmm1\n\t"
        "movq active_trampoline(%rip), %rax\n\t"
        "call *%rax\n\t"
        "movd %xmm0, %eax\n\t"
        "ret\n\t"
    );
}

static inline void workload_once(void) {
    (void)simd_shim(42, 7);
    __atomic_fetch_add(&workload_iterations, 1, __ATOMIC_RELAXED);
}

// ============================================================================
// 4. PERFORMANCE MONITORING
// ============================================================================

typedef struct {
    uint64_t nr;
    uint64_t time_enabled;
    uint64_t time_running;
    uint64_t values[2];
} perf_group_read_t;

typedef enum {
    PERF_MODE_NONE = 0,
    PERF_MODE_HARDWARE,
    PERF_MODE_SOFTWARE
} perf_mode_t;

typedef struct perf_ctx {
    int fd_cycles;
    int fd_insns;
    int fd_llc_misses;
    uint64_t baseline_cpi;
    uint64_t baseline_llc_mpki_milli;
    uint64_t slow_cpi;
    uint64_t fast_cpi;
    uint64_t slow_llc_mpki;
    uint64_t fast_llc_mpki;
    uint32_t ratio_history[RATIO_HISTORY];
    size_t   ratio_history_count;
    size_t   ratio_history_cursor;
    uint32_t ratio_trimmed_milli;
    int pinned_cpu;
    int monitor_cpu;
    perf_group_read_t last_group_read;
    int last_group_valid;
    uint64_t last_llc_value;
    perf_mode_t mode;
    int software_adaptation;
    struct timespec sw_last_timestamp;
    uint64_t sw_last_iterations;
} perf_ctx_t;

typedef struct {
    uint64_t cpi_milli;
    uint32_t ratio_milli;
    uint32_t trimmed_ratio_milli;
    uint64_t llc_mpki_milli;
    uint64_t severity_milli;
    int memory_bound;
} thermal_eval_t;

static long perf_event_open_sys(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static void
#ifdef TSD_ENABLE_TESTS
__attribute__((unused))
#endif
enable_perf(perf_ctx_t* ctx) {
    if (!ctx || ctx->mode != PERF_MODE_HARDWARE) return;
    try_perf_ioctl(ctx->fd_cycles, PERF_EVENT_IOC_RESET, "perf ioctl reset(cycles)");
    try_perf_ioctl(ctx->fd_cycles, PERF_EVENT_IOC_ENABLE, "perf ioctl enable(cycles)");
    try_perf_ioctl(ctx->fd_insns, PERF_EVENT_IOC_RESET, "perf ioctl reset(insns)");
    try_perf_ioctl(ctx->fd_insns, PERF_EVENT_IOC_ENABLE, "perf ioctl enable(insns)");
    try_perf_ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_RESET, "perf ioctl reset(llc)");
    try_perf_ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_ENABLE, "perf ioctl enable(llc)");
}

static void
#ifdef TSD_ENABLE_TESTS
__attribute__((unused))
#endif
disable_perf(perf_ctx_t* ctx) {
    if (!ctx || ctx->mode != PERF_MODE_HARDWARE) return;
    try_perf_ioctl(ctx->fd_cycles, PERF_EVENT_IOC_DISABLE, "perf ioctl disable(cycles)");
    try_perf_ioctl(ctx->fd_insns, PERF_EVENT_IOC_DISABLE, "perf ioctl disable(insns)");
    try_perf_ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_DISABLE, "perf ioctl disable(llc)");
}

static perf_ctx_t* init_perf_monitoring(void) {
    perf_ctx_t *ctx = calloc(1, sizeof(perf_ctx_t));
    if (!ctx) return NULL;
    ctx->fd_cycles = -1;
    ctx->fd_insns = -1;
    ctx->fd_llc_misses = -1;
    ctx->last_group_valid = 0;
    ctx->last_llc_value = 0;
    ctx->mode = PERF_MODE_NONE;
    ctx->software_adaptation = 0;
    ctx->sw_last_iterations = 0;
    ctx->sw_last_timestamp.tv_sec = 0;
    ctx->sw_last_timestamp.tv_nsec = 0;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    (void)sched_setaffinity(0, sizeof(cpuset), &cpuset);
    ctx->pinned_cpu = 0;
    ctx->monitor_cpu = 0;
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count > 1) {
        ctx->monitor_cpu = (ctx->pinned_cpu + 1) % (int)cpu_count;
        if (ctx->monitor_cpu == ctx->pinned_cpu) {
            ctx->monitor_cpu = ctx->pinned_cpu;
        }
    }

    const char *force_sw_env = getenv("TSD_FAKE_PERF");
    int force_sw = force_sw_env && force_sw_env[0] != '\0' && strcmp(force_sw_env, "0") != 0;

    if (force_sw) {
        ctx->mode = PERF_MODE_SOFTWARE;
        return ctx;
    }

    struct perf_event_attr pe = {0};
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(pe);
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    // CPU cycles (group leader). Attach to this process on the pinned core.
    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    long fd_cycles = perf_event_open_sys(&pe, 0, ctx->pinned_cpu, -1, 0);
    if (fd_cycles < 0) {
        int err = errno;
        if (should_fallback_to_software(err)) {
            log_errno_message("perf_event_open cycles (falling back to software)", err);
            ctx->mode = PERF_MODE_SOFTWARE;
            return ctx;
        }
        log_errno_message("perf_event_open cycles", err);
        free(ctx);
        return NULL;
    }
    ctx->fd_cycles = (int)fd_cycles;

    // Instructions (in the same per-CPU group)
    pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    long fd_insns = perf_event_open_sys(&pe, 0, ctx->pinned_cpu, ctx->fd_cycles, 0);
    if (fd_insns < 0) {
        int err = errno;
        close(ctx->fd_cycles);
        ctx->fd_cycles = -1;
        if (should_fallback_to_software(err)) {
            log_errno_message("perf_event_open instructions (falling back to software)", err);
            ctx->mode = PERF_MODE_SOFTWARE;
            return ctx;
        }
        log_errno_message("perf_event_open instructions", err);
        free(ctx);
        return NULL;
    }
    ctx->fd_insns = (int)fd_insns;

    // LLC misses (separate counter, also bound to the pinned CPU)
    pe.read_format = 0;
    pe.config = PERF_COUNT_HW_CACHE_MISSES;
    long fd_llc_misses = perf_event_open_sys(&pe, 0, ctx->pinned_cpu, -1, 0);
    if (fd_llc_misses < 0) {
        int err = errno;
        ctx->fd_llc_misses = -1;
        if (!warned_llc_unavailable) {
            fprintf(stderr,
                    "warning: LLC miss counter unavailable (perf_event_open: %s); memory-bound guard disabled\n",
                    strerror(err));
            warned_llc_unavailable = 1;
        }
    } else {
        ctx->fd_llc_misses = (int)fd_llc_misses;
    }

    ctx->mode = PERF_MODE_HARDWARE;
    return ctx;
}

static void cleanup_perf(perf_ctx_t *ctx) {
    if (!ctx) return;
    disable_perf(ctx);
    if (ctx->fd_cycles >= 0) close(ctx->fd_cycles);
    if (ctx->fd_insns  >= 0) close(ctx->fd_insns);
    if (ctx->fd_llc_misses >= 0) close(ctx->fd_llc_misses);
    free(ctx);
}

// Scale helper (copied signature used above). 128-bit to avoid overflow.
static inline uint64_t scale_counter(uint64_t delta, uint64_t time_enabled, uint64_t time_running) {
    if (time_running == 0 || time_enabled == 0) return delta;
    __uint128_t num = (__uint128_t)delta * (__uint128_t)time_enabled;
    return (uint64_t)(num / ( __uint128_t)time_running);
}

static void measure_baseline_cpi(perf_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->mode == PERF_MODE_SOFTWARE) {
        struct timespec start = {0}, end = {0};
        const int loops = 100000;
        uint64_t before_iters = __atomic_load_n(&workload_iterations, __ATOMIC_RELAXED);
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < loops; i++) workload_once();
        clock_gettime(CLOCK_MONOTONIC, &end);
        uint64_t after_iters = __atomic_load_n(&workload_iterations, __ATOMIC_RELAXED);
        uint64_t delta_iters = (after_iters > before_iters) ? (after_iters - before_iters) : (uint64_t)loops;
        uint64_t elapsed_ns = timespec_diff_ns(&start, &end);
        if (elapsed_ns == 0) {
            elapsed_ns = 1;
        }
        uint64_t surrogate_cpi = (delta_iters == 0) ? 1000 : ((elapsed_ns * 1000ULL) / delta_iters);
        if (surrogate_cpi == 0) {
            surrogate_cpi = 1000;
        }
        ctx->baseline_cpi = surrogate_cpi;
        ctx->baseline_llc_mpki_milli = 1000;
        ctx->slow_cpi = ctx->fast_cpi = ctx->baseline_cpi;
        ctx->slow_llc_mpki = ctx->fast_llc_mpki = ctx->baseline_llc_mpki_milli;
        ctx->ratio_history_count = 0;
        ctx->ratio_history_cursor = 0;
        ctx->ratio_trimmed_milli = (param_down_ratio_milli > UINT32_MAX) ? UINT32_MAX : (uint32_t)param_down_ratio_milli;
        ctx->last_group_valid = 0;
        ctx->last_llc_value = 0;
        ctx->software_adaptation = 1;
        ctx->sw_last_iterations = __atomic_load_n(&workload_iterations, __ATOMIC_RELAXED);
        clock_gettime(CLOCK_MONOTONIC, &ctx->sw_last_timestamp);
        printf("Baseline (software) CPI surrogate: %lu.%03lu\n", surrogate_cpi / 1000, surrogate_cpi % 1000);
        printf("Baseline MPKI surrogate: %lu.%03lu\n", ctx->baseline_llc_mpki_milli / 1000, ctx->baseline_llc_mpki_milli % 1000);
        return;
    }
    perf_group_read_t rd_before = {0}, rd_after = {0};
    uint64_t llc_before = 0, llc_after = 0;
    ssize_t n = read(ctx->fd_cycles, &rd_before, sizeof(rd_before));
    if (n < (ssize_t)sizeof(rd_before) || rd_before.nr != 2) { ctx->baseline_cpi = 1000; return; }
    if (ctx->fd_llc_misses >= 0) {
        ssize_t llc_read = read(ctx->fd_llc_misses, &llc_before, sizeof(llc_before));
        if (llc_read < 0) llc_before = 0;
    }
    for (int i = 0; i < 100000; i++) workload_once();
    n = read(ctx->fd_cycles, &rd_after, sizeof(rd_after));
    if (n < (ssize_t)sizeof(rd_after) || rd_after.nr != 2) { ctx->baseline_cpi = 1000; return; }
    if (ctx->fd_llc_misses >= 0) {
        ssize_t llc_read = read(ctx->fd_llc_misses, &llc_after, sizeof(llc_after));
        if (llc_read < 0) llc_after = llc_before;
    }
    uint64_t delta_cycles = scale_counter(rd_after.values[0] - rd_before.values[0],
                                          rd_after.time_enabled - rd_before.time_enabled,
                                          rd_after.time_running - rd_before.time_running);
    uint64_t delta_insns  = scale_counter(rd_after.values[1] - rd_before.values[1],
                                          rd_after.time_enabled - rd_before.time_enabled,
                                          rd_after.time_running - rd_before.time_running);
    ctx->baseline_cpi = (delta_cycles * 1000) / (delta_insns ?: 1);
    uint64_t delta_llc = (ctx->fd_llc_misses >= 0 && llc_after >= llc_before) ? (llc_after - llc_before) : 0;
    ctx->baseline_llc_mpki_milli = delta_insns ? (delta_llc * MPKI_SCALE) / delta_insns : 0;
    if (ctx->baseline_llc_mpki_milli == 0) {
        ctx->baseline_llc_mpki_milli = 1000; // Assume light cache pressure if we saw none.
    }
    ctx->slow_cpi = ctx->fast_cpi = ctx->baseline_cpi ?: 1000;
    ctx->slow_llc_mpki = ctx->fast_llc_mpki = ctx->baseline_llc_mpki_milli;
    ctx->ratio_history_count = 0;
    ctx->ratio_history_cursor = 0;
    ctx->ratio_trimmed_milli = (param_down_ratio_milli > UINT32_MAX) ? UINT32_MAX : (uint32_t)param_down_ratio_milli;
    ctx->last_group_read = rd_after;
    ctx->last_group_valid = 1;
    ctx->last_llc_value = llc_after;
    printf("Baseline CPI: %lu.%03lu\n", ctx->baseline_cpi / 1000, ctx->baseline_cpi % 1000);
    printf("Baseline MPKI: %lu.%03lu\n", ctx->baseline_llc_mpki_milli / 1000, ctx->baseline_llc_mpki_milli % 1000);
}


static int process_measurement(perf_ctx_t *ctx, thermal_eval_t *out, uint64_t current_cpi, uint64_t mpki_milli) {
    if (!ctx) return 0;
    ctx->fast_cpi = tsd_update_ewma(ctx->fast_cpi, current_cpi, FAST_EWMA_SHIFT);
    ctx->slow_cpi = tsd_update_ewma(ctx->slow_cpi, current_cpi, SLOW_EWMA_SHIFT);
    if (ctx->slow_cpi == 0) ctx->slow_cpi = current_cpi ?: (ctx->baseline_cpi ?: 1000);
    ctx->fast_llc_mpki = tsd_update_ewma(ctx->fast_llc_mpki, mpki_milli, FAST_EWMA_SHIFT);
    ctx->slow_llc_mpki = tsd_update_ewma(ctx->slow_llc_mpki, mpki_milli, SLOW_EWMA_SHIFT);

    uint64_t reference_cpi = ctx->slow_cpi ?: (ctx->baseline_cpi ?: 1);
    __uint128_t ratio_num = (__uint128_t)current_cpi * 1000u;
    uint64_t ratio_milli = (uint64_t)((ratio_num + reference_cpi / 2) / reference_cpi);
    if (ctx->ratio_history_count < RATIO_HISTORY) {
        ctx->ratio_history_count++;
    }
    uint32_t stored_ratio = (ratio_milli > UINT32_MAX) ? UINT32_MAX : (uint32_t)ratio_milli;
    ctx->ratio_history[ctx->ratio_history_cursor] = stored_ratio;
    ctx->ratio_history_cursor = (ctx->ratio_history_cursor + 1) % RATIO_HISTORY;
    ctx->ratio_trimmed_milli = tsd_compute_trimmed_mean(ctx->ratio_history, ctx->ratio_history_count);

    uint64_t baseline_mpki = ctx->baseline_llc_mpki_milli ?: 1000;
    uint64_t mpki_reference = ctx->slow_llc_mpki ?: mpki_milli;
    __uint128_t mpki_ratio_num = (__uint128_t)mpki_reference * 1000u;
    uint64_t mpki_ratio = (uint64_t)((mpki_ratio_num + baseline_mpki / 2) / baseline_mpki);
    int memory_bound = ctx->fd_llc_misses >= 0 && mpki_ratio > 2500;

    uint64_t dynamic_threshold = param_down_ratio_milli;
    if (ctx->fast_cpi > ctx->slow_cpi) {
        uint64_t delta = ctx->fast_cpi - ctx->slow_cpi;
        uint64_t delta_ratio = (uint64_t)(((__uint128_t)delta * 1000u) / (ctx->slow_cpi ?: 1));
        uint64_t slope_penalty = delta_ratio / 5;
        if (slope_penalty > dynamic_threshold / 4) {
            slope_penalty = dynamic_threshold / 4;
        }
        if (dynamic_threshold > slope_penalty) {
            dynamic_threshold -= slope_penalty;
        }
    }
    if (memory_bound) {
        uint64_t guard = 0;
        if (param_memory_guard_divisor > 0) {
            guard = dynamic_threshold / (uint64_t)param_memory_guard_divisor;
        }
        guard += (uint64_t)param_memory_guard_offset_milli;
        dynamic_threshold += guard;
    }

    uint64_t consensus_ratio = (ratio_milli + ctx->ratio_trimmed_milli) / 2;
    uint64_t severity = (consensus_ratio > dynamic_threshold) ? (consensus_ratio - dynamic_threshold) : 0;

    if (out) {
        out->cpi_milli = current_cpi;
        out->ratio_milli = (uint32_t)ratio_milli;
        out->trimmed_ratio_milli = ctx->ratio_trimmed_milli;
        out->llc_mpki_milli = mpki_milli;
        out->severity_milli = severity;
        out->memory_bound = memory_bound;
    }

    return severity > 0;
}

static int evaluate_thermal_state(perf_ctx_t *ctx, thermal_eval_t *out) {
    if (!ctx) return 0;
    if (ctx->mode == PERF_MODE_SOFTWARE) {
#ifdef TSD_ENABLE_TESTS
        if (test_perf_script.enabled && test_perf_script.count > 0) {
            uint32_t ratio = test_perf_script.ratios[test_perf_script.index];
            if (test_perf_script.index + 1 < test_perf_script.count) {
                test_perf_script.index++;
            }
            uint64_t baseline = ctx->slow_cpi ?: (ctx->baseline_cpi ?: 1000);
            if (baseline == 0) {
                baseline = 1000;
            }
            uint64_t current_cpi = (uint64_t)(((__uint128_t)baseline * ratio + 500u) / 1000u);
            return process_measurement(ctx, out, current_cpi, test_perf_script.mpki);
        }
#endif
        if (!ctx->software_adaptation) {
            ctx->sw_last_iterations = __atomic_load_n(&workload_iterations, __ATOMIC_RELAXED);
            clock_gettime(CLOCK_MONOTONIC, &ctx->sw_last_timestamp);
            ctx->software_adaptation = 1;
            return 0;
        }
        struct timespec now = {0};
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_iters = __atomic_load_n(&workload_iterations, __ATOMIC_RELAXED);
        uint64_t delta_iters = (now_iters >= ctx->sw_last_iterations) ? (now_iters - ctx->sw_last_iterations) : 0;
        uint64_t delta_ns = timespec_diff_ns(&ctx->sw_last_timestamp, &now);
        ctx->sw_last_iterations = now_iters;
        ctx->sw_last_timestamp = now;
        if (delta_iters == 0 || delta_ns == 0) {
            return 0;
        }
        uint64_t current_cpi = (delta_ns * 1000ULL) / delta_iters;
        if (current_cpi == 0) {
            current_cpi = ctx->baseline_cpi ?: 1000;
        }
        return process_measurement(ctx, out, current_cpi, 0);
    }

    perf_group_read_t rd_now = {0};
    uint64_t llc_now = 0;
    ssize_t n = read(ctx->fd_cycles, &rd_now, sizeof(rd_now));
    if (n >= (ssize_t)sizeof(rd_now) && rd_now.nr != 2 && !warned_perf_group_layout) {
        fprintf(stderr,
                "warning: perf group returned %" PRIu64 " counters (expected 2); cycle telemetry disabled\n",
                (uint64_t)rd_now.nr);
        warned_perf_group_layout = 1;
    }
    if (n < (ssize_t)sizeof(rd_now) || rd_now.nr != 2) {
        ctx->last_group_valid = 0;
        return 0;
    }
    if (ctx->fd_llc_misses >= 0) {
        ssize_t llc_read = read(ctx->fd_llc_misses, &llc_now, sizeof(llc_now));
        if (llc_read < 0) llc_now = ctx->last_llc_value;
    }
    if (!ctx->last_group_valid) {
        ctx->last_group_read = rd_now;
        ctx->last_group_valid = 1;
        ctx->last_llc_value = llc_now;
        return 0;
    }

    perf_group_read_t rd_before = ctx->last_group_read;
    uint64_t llc_before = ctx->last_llc_value;
    ctx->last_group_read = rd_now;
    ctx->last_llc_value = llc_now;

    uint64_t delta_cycles = scale_counter(rd_now.values[0] - rd_before.values[0],
                                          rd_now.time_enabled - rd_before.time_enabled,
                                          rd_now.time_running - rd_before.time_running);
    uint64_t delta_insns  = scale_counter(rd_now.values[1] - rd_before.values[1],
                                          rd_now.time_enabled - rd_before.time_enabled,
                                          rd_now.time_running - rd_before.time_running);
    uint64_t current_cpi = (delta_cycles * 1000) / (delta_insns ?: 1);
    uint64_t delta_llc = (ctx->fd_llc_misses >= 0 && llc_now >= llc_before)
                         ? (llc_now - llc_before) : 0;
    uint64_t mpki_milli = delta_insns ? (delta_llc * MPKI_SCALE) / delta_insns : 0;

    return process_measurement(ctx, out, current_cpi, mpki_milli);
}


// ============================================================================
// 5. ADAPTIVE MANAGER
// ============================================================================

_Atomic int running = 1;

#ifdef TSD_ENABLE_TESTS
static void tsd_test_clear_fake_perf_script_internal(void) {
    test_perf_script.enabled = 0;
    test_perf_script.count = 0;
    test_perf_script.index = 0;
    test_perf_script.mpki = 0;
    memset(test_perf_script.ratios, 0, sizeof(test_perf_script.ratios));
}

void tsd_test_clear_fake_perf_script(void) {
    tsd_test_clear_fake_perf_script_internal();
}

void tsd_test_set_fake_perf_script(const uint32_t *ratios, size_t count, uint32_t mpki) {
    tsd_test_clear_fake_perf_script_internal();
    if (!ratios || count == 0) {
        return;
    }
    if (count > sizeof(test_perf_script.ratios) / sizeof(test_perf_script.ratios[0])) {
        count = sizeof(test_perf_script.ratios) / sizeof(test_perf_script.ratios[0]);
    }
    for (size_t i = 0; i < count; ++i) {
        test_perf_script.ratios[i] = ratios[i];
    }
    test_perf_script.count = count;
    test_perf_script.index = 0;
    test_perf_script.mpki = mpki;
    test_perf_script.enabled = 1;
}

void tsd_test_clear_patch_overrides(void) {
    for (size_t i = 0; i < 3; ++i) {
        test_patch_override[i] = NULL;
        test_patch_override_size[i] = 0;
    }
}

void tsd_test_override_patch(simd_width_t width, const uint8_t *bytes, size_t len) {
    if (width < SIMD_SSE41 || width > SIMD_AVX512) {
        return;
    }
    test_patch_override[width] = bytes;
    test_patch_override_size[width] = len;
}

const uint8_t* tsd_test_patch_bytes(simd_width_t width, size_t *len) {
    if (len) {
        *len = 0;
    }
    switch (width) {
        case SIMD_SSE41:
            if (len) *len = sizeof(PATCH_SSE41);
            return PATCH_SSE41;
        case SIMD_AVX2:
            if (len) *len = sizeof(PATCH_AVX2);
            return PATCH_AVX2;
        case SIMD_AVX512:
            if (len) *len = sizeof(PATCH_AVX512);
            return PATCH_AVX512;
        default:
            return NULL;
    }
}

void tsd_test_set_detect_override(simd_width_t (*fn)(void)) {
    tsd_test_detect_hook = fn;
}

void tsd_test_clear_detect_override(void) {
    tsd_test_detect_hook = NULL;
}

void tsd_test_force_patch_failure(tsd_patch_fail_stage_t stage) {
    test_patch_fail_stage = stage;
}

const char* tsd_test_last_patch_error(void) {
    return test_last_patch_error;
}

void tsd_test_run_workload(int iterations) {
    if (iterations < 0) {
        return;
    }
    for (int i = 0; i < iterations; ++i) {
        workload_once();
    }
}

void tsd_test_reset_workload_counter(void) {
    __atomic_store_n(&workload_iterations, 0, __ATOMIC_RELAXED);
}

void tsd_test_set_running(int value) {
    __atomic_store_n(&running, value, __ATOMIC_RELEASE);
}

simd_width_t tsd_test_current_width(void) {
    return __atomic_load_n(&current_width, __ATOMIC_RELAXED);
}

unsigned char tsd_test_last_patched_width(void) {
    return __atomic_load_n(&last_patched_width, __ATOMIC_RELAXED);
}

void tsd_test_patch(simd_width_t width) {
    atomic_patch_strict_wx(width);
}

perf_ctx_t* tsd_test_init_perf(void) {
    return init_perf_monitoring();
}

void tsd_test_measure_baseline(perf_ctx_t *ctx) {
    measure_baseline_cpi(ctx);
}

void tsd_test_cleanup_perf(perf_ctx_t *ctx) {
    cleanup_perf(ctx);
}

simd_width_t tsd_test_detect_host_max(void) {
    simd_width_t (*saved)(void) = tsd_test_detect_hook;
    tsd_test_detect_hook = NULL;
    simd_width_t result = detect_max_simd();
    tsd_test_detect_hook = saved;
    return result;
}

void tsd_test_set_policy_counts(int down, int up) {
    if (down > 0) {
        param_down_count = down;
    }
    if (up > 0) {
        param_up_count = up;
    }
}

void tsd_test_set_timing(int interval_us, int cooldown_down_ms, int cooldown_up_ms, int dwell_ms) {
    if (interval_us > 0) {
        param_check_interval_us = interval_us;
    }
    if (cooldown_down_ms >= 0) {
        param_cooldown_down_ms = cooldown_down_ms;
    }
    if (cooldown_up_ms >= 0) {
        param_cooldown_up_ms = cooldown_up_ms;
    }
    if (dwell_ms >= 0) {
        param_min_dwell_ms = dwell_ms;
    }
}

int tsd_test_refresh_ticks(void) {
    return refresh_tick_config();
}

static void tsd_test_reset_patch_state(void) {
    __atomic_store_n(&current_width, SIMD_SSE41, __ATOMIC_RELAXED);
    __atomic_store_n(&current_width_byte, (unsigned char)SIMD_SSE41, __ATOMIC_RELAXED);
    __atomic_store_n(&trampoline_initialized, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&active_trampoline, trampoline_ctx.active, __ATOMIC_SEQ_CST);
    __atomic_store_n(&last_patch_attempt, (unsigned char)SIMD_SSE41, __ATOMIC_RELAXED);
    __atomic_store_n(&last_patched_width, (unsigned char)SIMD_SSE41, __ATOMIC_RELAXED);
    test_last_patch_error[0] = '\0';
    test_patch_fail_stage = TSD_PATCH_FAIL_NONE;
}

void tsd_test_reset_runtime(void) {
    param_check_interval_us = TSD_DEFAULT_CHECK_INTERVAL_US;
    param_down_count = TSD_DEFAULT_DOWN_COUNT;
    param_up_count = TSD_DEFAULT_UP_COUNT;
    param_down_ratio = TSD_DEFAULT_DOWN_RATIO;
    param_down_ratio_milli = TSD_DEFAULT_DOWN_RATIO_MILLI;
    param_cooldown_down_ms = TSD_DEFAULT_COOLDOWN_DOWN_MS;
    param_cooldown_up_ms = TSD_DEFAULT_COOLDOWN_UP_MS;
    param_allow_avx512 = TSD_DEFAULT_ALLOW_AVX512;
    param_min_dwell_ms = TSD_DEFAULT_MIN_DWELL_MS;
    param_memory_guard_divisor = TSD_DEFAULT_MEMORY_GUARD_DIVISOR;
    param_memory_guard_offset_milli = TSD_DEFAULT_MEMORY_GUARD_OFFSET_MILLI;
    demo_duration_sec = TSD_DEFAULT_DEMO_DURATION_SEC;
    work_iters = TSD_DEFAULT_WORK_ITERS;
    warned_llc_unavailable = 0;
    warned_perf_group_layout = 0;
    g_avx_available = 0;
    tsd_test_clear_detect_override();
    tsd_test_clear_patch_overrides();
    tsd_test_clear_fake_perf_script_internal();
    tsd_test_reset_patch_state();
    __atomic_store_n(&running, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&workload_iterations, 0, __ATOMIC_RELAXED);
    refresh_tick_config();
}
#endif

void* thermal_monitor_thread(void *arg) {
    perf_ctx_t *ctx = (perf_ctx_t*)arg;
    simd_width_t width = __atomic_load_n(&current_width, __ATOMIC_ACQUIRE);
    simd_width_t max_width_cached = detect_max_simd();
    int throttle_count = 0, stable_count = 0, cooldown = 0, dwell_ticks = 0;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET((size_t)ctx->monitor_cpu, &cpuset);
    (void)sched_setaffinity(0, sizeof(cpuset), &cpuset);
    while (__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
        struct timespec interval = {
            .tv_sec = param_check_interval_us / 1000000,
            .tv_nsec = (long)(param_check_interval_us % 1000000) * 1000L,
        };
        while (nanosleep(&interval, &interval) == -1 && errno == EINTR) {
        }
        dwell_ticks++;
        if (cooldown > 0) { cooldown--; continue; }
        if (dwell_ticks < min_dwell_ticks) { continue; }
        thermal_eval_t eval = {0};
        if (evaluate_thermal_state(ctx, &eval)) {
            throttle_count++; stable_count = 0;
            if (throttle_count >= param_down_count && width > SIMD_SSE41) {
                printf("\nThermal throttle: ratio=%u.%03u (trimmed %u.%03u) severity=+%lu.%03lu MPKI=%lu.%03lu%s\n",
                       eval.ratio_milli / 1000, eval.ratio_milli % 1000,
                       eval.trimmed_ratio_milli / 1000, eval.trimmed_ratio_milli % 1000,
                       eval.severity_milli / 1000, eval.severity_milli % 1000,
                       eval.llc_mpki_milli / 1000, eval.llc_mpki_milli % 1000,
                       eval.memory_bound ? " [memory bound guard raised]" : "");
                width--; atomic_patch_strict_wx(width); throttle_count = 0; cooldown = cooldown_down_ticks; dwell_ticks = 0;
            }
        } else {
            stable_count++; throttle_count = 0;
            if (stable_count >= param_up_count && width < max_width_cached) {
                printf("\nRecovered: ratio=%u.%03u (trimmed %u.%03u) MPKI=%lu.%03lu\n",
                       eval.ratio_milli / 1000, eval.ratio_milli % 1000,
                       eval.trimmed_ratio_milli / 1000, eval.trimmed_ratio_milli % 1000,
                       eval.llc_mpki_milli / 1000, eval.llc_mpki_milli % 1000);
                width++; atomic_patch_strict_wx(width); stable_count = 0; cooldown = cooldown_up_ticks; dwell_ticks = 0;
            }
        }
    }
    return NULL;
}


// ============================================================================
// 6. MAIN
// ============================================================================

#ifndef TSD_ENABLE_TESTS
int main(int argc, char **argv) {
    printf("=== Production Thermal-Aware SIMD Dispatcher ===\\n\\n");
    parse_flags(argc, argv);
    install_signal_handlers();
    __atomic_store_n(&last_patch_attempt, (unsigned char)SIMD_SSE41, __ATOMIC_RELAXED);
    __atomic_store_n(&last_patched_width, (unsigned char)SIMD_SSE41, __ATOMIC_RELAXED);
    cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(0, &cpuset); sched_setaffinity(0, sizeof(cpuset), &cpuset);
    simd_width_t max_width = detect_max_simd();
    if (max_width == SIMD_SSE41 && !cpu_has_sse41()) { fprintf(stderr, "ERROR: SSE4.1 required but not available\\n"); return 1; }
    printf("Maximum supported: %s%s\\n", max_width == SIMD_AVX512 ? "AVX-512 (XMM-only)" : max_width == SIMD_AVX2 ? "AVX2 (XMM-only)" : "SSE4.1", !param_allow_avx512 ? " [AVX-512 disabled by policy]" : "");
    printf("AVX transition guard: %s\\n", g_avx_available ? "enabled" : "disabled");
    printf("Configuration:\\n");
    printf("  Check interval: %d ms\\n", param_check_interval_us / 1000);
    printf("  Down threshold: %.1fx CPI (after %d events)\\n", param_down_ratio, param_down_count);
    printf("  Up threshold: %d stable events\\n", param_up_count);
    printf("  Cooldown: %d ms down, %d ms up\\n", param_cooldown_down_ms, param_cooldown_up_ms);
    printf("  Minimum dwell: %d ms per width\\n", param_min_dwell_ms);
    printf("  Memory guard: divisor=%d offset=%d milli\\n", param_memory_guard_divisor, param_memory_guard_offset_milli);
    printf("  Cooldown ticks: down=%d up=%d min-dwell=%d\\n",
           cooldown_down_ticks, cooldown_up_ticks, min_dwell_ticks);
    printf("  Demo: %d sec, work iters: %d\\n\\n", demo_duration_sec, work_iters);
    if (init_double_buffer_trampoline() != 0) { fprintf(stderr, "Failed to create trampolines\\n"); return 1; }
    atomic_patch_strict_wx(max_width);
    perf_ctx_t *perf = init_perf_monitoring();
    if (perf) {
        if (perf->mode == PERF_MODE_HARDWARE) {
            enable_perf(perf);
            measure_baseline_cpi(perf);
            printf("Perf target CPU: %d (monitor thread on CPU %d)\\n", perf->pinned_cpu, perf->monitor_cpu);
        } else {
            printf("\nHardware performance counters unavailable; using software telemetry fallback.\\n");
            measure_baseline_cpi(perf);
        }
        pthread_t monitor;
        int monitor_started = 0;
        int monitor_err = pthread_create(&monitor, NULL, thermal_monitor_thread, perf);
        if (monitor_err != 0) {
            fprintf(stderr, "ERROR: Failed to start monitor thread: %s\n", strerror(monitor_err));
            __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
            cleanup_perf(perf);
            return 1;
        }
        monitor_started = 1;
        printf("\\nRunning workload for %d seconds...\\n", demo_duration_sec);
        printf("Try: stress-ng --cpu 8 --cpu-load 100  (to simulate thermal load)\\n\\n");
        for (int sec = 0; sec < demo_duration_sec; sec++) {
            for (int i = 0; i < work_iters; i++) workload_once();
            printf("."); fflush(stdout);
        }
        printf("\\n\\nDone.\\n");
        __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
        if (monitor_started) {
            pthread_join(monitor, NULL);
        }
        cleanup_perf(perf);
    } else {
        printf("\\nPerformance monitoring unavailable.\\n");
        printf("Suggestions:\\n");
        printf("  - Run as root: sudo ./thermal_simd\\n");
        printf("  - Or: sudo sysctl kernel.perf_event_paranoid=0\\n");
        printf("  - Container: add --cap-add=SYS_ADMIN or --privileged\\n\\n");
        printf("Running without thermal adaptation...\\n");
        for (int i = 0; i < 100000000; i++) workload_once();
    }
    return 0;
}
#endif
