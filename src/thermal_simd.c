#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <asm/unistd.h>
#include <cpuid.h>

// ============================================================================
// 0. RUNTIME CONFIGURATION (CLI-tunable)
// ============================================================================

static int param_check_interval_us = 50000;     // Check every 50ms
static int param_down_count = 3;                // Downgrade after 3 throttles
static int param_up_count = 5;                  // Upgrade after 5 stable
static double param_down_ratio = 1.5;           // Throttle threshold (1.5x baseline CPI)
static uint64_t param_down_ratio_milli = 1500;  // Scaled ratio (x1000) for integer math
static int param_cooldown_down_ms = 1000;       // 1s cooldown after downgrade
static int param_cooldown_up_ms = 2000;         // 2s cooldown after upgrade
static int param_allow_avx512 = 1;              // Allow AVX-512 (disable for Zen or conservative policy)
static int param_min_dwell_ms = 200;            // Minimum 200ms per width (prevents rapid flipping)

#define RATIO_HISTORY 8
#define FAST_EWMA_SHIFT 2
#define SLOW_EWMA_SHIFT 5
#define MPKI_SCALE 1000000ULL

// Demo controls
static int demo_duration_sec = 10;              // --duration-sec
static int work_iters = 10000000;               // --work-iters

// Computed tick values (set after CLI parsing)
static int cooldown_down_ticks;
static int cooldown_up_ticks;
static int min_dwell_ticks;

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Options:\n");
    printf("  --interval=MS          Check interval in milliseconds (default: 50)\n");
    printf("  --down-count=N         Throttle events before downgrade (default: 3)\n");
    printf("  --up-count=N           Stable events before upgrade (default: 5)\n");
    printf("  --down-ratio=R         CPI ratio for throttle detection (default: 1.5)\n");
    printf("  --cooldown-down=MS     Cooldown after downgrade (default: 1000)\n");
    printf("  --cooldown-up=MS       Cooldown after upgrade (default: 2000)\n");
    printf("  --min-dwell=MS         Minimum time per width (default: 200)\n");
    printf("  --no-avx512            Cap at AVX2 (for Zen or conservative policy)\n");
    printf("  --duration-sec=S       Demo duration (default: 10)\n");
    printf("  --work-iters=N         Inner work iterations per second (default: 10000000)\n");
    printf("  --help                 Show this help\n");
}

static void die_invalid_option(const char *option, const char *value) {
    fprintf(stderr, "Invalid value for %s: '%s'\n", option, value);
    exit(1);
}

static int parse_int_option(const char *value, long min, long max, int *out) {
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

static int parse_ms_option(const char *value, int min_ms, int max_ms, int *out_us) {
    int parsed_ms;
    if (parse_int_option(value, min_ms, max_ms, &parsed_ms) != 0) {
        return -1;
    }
    if ((long)parsed_ms * 1000L > INT_MAX) {
        return -1;
    }
    *out_us = parsed_ms * 1000;
    return 0;
}

static int parse_ratio_option(const char *value, double min, double max, double *ratio_out, uint64_t *scaled_out) {
    if (!value || !ratio_out || !scaled_out) return -1;
    errno = 0;
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0') {
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
    return 0;
}

static int compute_ticks_from_ms(int ms, const char *flag_name) {
    int64_t interval_us = param_check_interval_us;
    if (interval_us <= 0) {
        fprintf(stderr, "Invalid sampling interval while processing %s\n", flag_name);
        exit(1);
    }

    int64_t total_us = (int64_t)ms * 1000;
    int64_t ticks64 = (total_us + interval_us - 1) / interval_us;
    if (ticks64 <= 0 || ticks64 > INT_MAX) {
        fprintf(stderr, "Value for %s results in unsupported tick count (%lld)\n",
                flag_name, (long long)ticks64);
        exit(1);
    }

    return (int)ticks64;
}

static void parse_flags(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--interval=", 11)) {
            if (parse_ms_option(argv[i] + 11, 1, 10000, &param_check_interval_us) != 0) {
                die_invalid_option("--interval", argv[i] + 11);
            }
        } else if (!strncmp(argv[i], "--down-count=", 13)) {
            if (parse_int_option(argv[i] + 13, 1, 100, &param_down_count) != 0) {
                die_invalid_option("--down-count", argv[i] + 13);
            }
        } else if (!strncmp(argv[i], "--up-count=", 11)) {
            if (parse_int_option(argv[i] + 11, 1, 100, &param_up_count) != 0) {
                die_invalid_option("--up-count", argv[i] + 11);
            }
        } else if (!strncmp(argv[i], "--down-ratio=", 13)) {
            if (parse_ratio_option(argv[i] + 13, 1.0, 10.0, &param_down_ratio, &param_down_ratio_milli) != 0) {
                die_invalid_option("--down-ratio", argv[i] + 13);
            }
        } else if (!strncmp(argv[i], "--cooldown-down=", 16)) {
            if (parse_int_option(argv[i] + 16, 1, 3600000, &param_cooldown_down_ms) != 0) {
                die_invalid_option("--cooldown-down", argv[i] + 16);
            }
        } else if (!strncmp(argv[i], "--cooldown-up=", 14)) {
            if (parse_int_option(argv[i] + 14, 1, 3600000, &param_cooldown_up_ms) != 0) {
                die_invalid_option("--cooldown-up", argv[i] + 14);
            }
        } else if (!strncmp(argv[i], "--min-dwell=", 12)) {
            if (parse_int_option(argv[i] + 12, 1, 3600000, &param_min_dwell_ms) != 0) {
                die_invalid_option("--min-dwell", argv[i] + 12);
            }
        } else if (!strcmp(argv[i], "--no-avx512")) {
            param_allow_avx512 = 0;
        } else if (!strncmp(argv[i], "--duration-sec=", 15)) {
            if (parse_int_option(argv[i] + 15, 1, 86400, &demo_duration_sec) != 0) {
                die_invalid_option("--duration-sec", argv[i] + 15);
            }
        } else if (!strncmp(argv[i], "--work-iters=", 13)) {
            if (parse_int_option(argv[i] + 13, 1, INT_MAX, &work_iters) != 0) {
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

    // Convert ms to ticks after parsing all flags (interval might change order)
    cooldown_down_ticks = compute_ticks_from_ms(param_cooldown_down_ms, "--cooldown-down");
    cooldown_up_ticks   = compute_ticks_from_ms(param_cooldown_up_ms, "--cooldown-up");
    min_dwell_ticks     = compute_ticks_from_ms(param_min_dwell_ms, "--min-dwell");
}

// ============================================================================
// 1. CORRECT FEATURE DETECTION (with AVX bit check)
// ============================================================================

typedef enum {
    SIMD_SSE41,
    SIMD_AVX2,
    SIMD_AVX512
} simd_width_t;

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

static uint8_t g_avx_available = 0; // Global for shim AVX transition guard

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
    uint8_t code[8];
} patch_slot_t __attribute__((aligned(8)));

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
static _Atomic(patch_slot_t*) active_trampoline __attribute__((aligned(8)));

// CORRECTED ENCODINGS (verified with objdump)
// XMM-only variants (128-bit, scalar-exact, minimal power/downclock)
static const uint8_t PATCH_SSE41[8]  = { 0x66,0x0F,0x38,0x40,0xC1, 0xC3,0x90,0x90 }; // pmulld xmm0,xmm1; ret
static const uint8_t PATCH_AVX2[8]   = { 0xC4,0xE2,0x79,0x40,0xC1, 0xC3,0x90,0x90 }; // vpmulld xmm0,xmm0,xmm1; ret
static const uint8_t PATCH_AVX512[8] = { 0x62,0xF2,0x79,0x08,0x40,0xC1, 0xC3,0x90 }; // vpmulld xmm0{k0},xmm0,xmm1; ret

_Static_assert(sizeof(PATCH_SSE41) == 8, "SSE41 payload must be 8 bytes");
_Static_assert(sizeof(PATCH_AVX2) == 8, "AVX2 payload must be 8 bytes");
_Static_assert(sizeof(PATCH_AVX512) == 8, "AVX512 payload must be 8 bytes");

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

int init_double_buffer_trampoline(void) {
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        return -1;
    }
    size_t pagesize = (size_t)pagesize_long;
    trampoline_ctx.page_a = create_rx_page();
    trampoline_ctx.page_b = create_rx_page();
    if (!trampoline_ctx.page_a || !trampoline_ctx.page_b) return -1;
    trampoline_ctx.active = (patch_slot_t*)trampoline_ctx.page_a;
    trampoline_ctx.inactive = (patch_slot_t*)trampoline_ctx.page_b;
    if (mprotect(trampoline_ctx.page_a, pagesize, PROT_READ | PROT_EXEC) != 0 ||
        mprotect(trampoline_ctx.page_b, pagesize, PROT_READ | PROT_EXEC) != 0)
        return -1;
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
    const uint8_t *patch_data;
    switch (new_width) {
        case SIMD_SSE41:  patch_data = PATCH_SSE41;  break;
        case SIMD_AVX2:   patch_data = PATCH_AVX2;   break;
        case SIMD_AVX512: patch_data = PATCH_AVX512; break;
        default: goto unlock;
    }
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        goto unlock;
    }
    size_t pagesize = (size_t)pagesize_long;
    patch_slot_t *inactive = trampoline_ctx.inactive;
    void *inactive_page = (void*)((uintptr_t)inactive & ~(pagesize - 1));
    if (mprotect(inactive_page, pagesize, PROT_READ | PROT_WRITE) != 0) goto unlock;
    uint64_t patch_qword; memcpy(&patch_qword, patch_data, 8);
    __atomic_store_n((uint64_t*)inactive->code, patch_qword, __ATOMIC_SEQ_CST);
    serialize_instruction_stream();
    if (mprotect(inactive_page, pagesize, PROT_READ | PROT_EXEC) != 0) goto unlock;
    __atomic_store_n(&active_trampoline, inactive, __ATOMIC_SEQ_CST);
    patch_slot_t *tmp = trampoline_ctx.active;
    trampoline_ctx.active = trampoline_ctx.inactive;
    trampoline_ctx.inactive = tmp;
    __atomic_store_n(&current_width, new_width, __ATOMIC_RELEASE);
    __atomic_store_n(&current_width_byte, (unsigned char)new_width, __ATOMIC_RELEASE);
    __atomic_store_n(&trampoline_initialized, 1, __ATOMIC_RELEASE);
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

static inline void workload_once(void) { (void)simd_shim(42, 7); }

// ============================================================================
// 4. PERFORMANCE MONITORING
// ============================================================================

typedef struct {
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
} perf_ctx_t;

typedef struct {
    uint64_t cpi_milli;
    uint32_t ratio_milli;
    uint32_t trimmed_ratio_milli;
    uint64_t llc_mpki_milli;
    uint64_t severity_milli;
    int memory_bound;
} thermal_eval_t;

static inline uint64_t update_ewma(uint64_t prev, uint64_t sample, unsigned shift) {
    if (prev == 0 || shift == 0) {
        return sample;
    }
    if (sample == prev) {
        return prev;
    }
    if (sample > prev) {
        uint64_t delta = sample - prev;
        uint64_t step = delta >> shift;
        if (step == 0) step = 1;
        uint64_t next = prev + step;
        return next > sample ? sample : next;
    }
    uint64_t delta = prev - sample;
    uint64_t step = delta >> shift;
    if (step == 0) step = 1;
    uint64_t next = prev - step;
    return next < sample ? sample : next;
}

static uint32_t compute_trimmed_mean(const uint32_t *values, size_t count) {
    if (count == 0) {
        return 0;
    }
    uint32_t scratch[RATIO_HISTORY];
    if (count > RATIO_HISTORY) count = RATIO_HISTORY;
    memcpy(scratch, values, count * sizeof(uint32_t));
    for (size_t i = 1; i < count; ++i) {
        uint32_t key = scratch[i];
        size_t j = i;
        while (j > 0 && scratch[j - 1] > key) {
            scratch[j] = scratch[j - 1];
            --j;
        }
        scratch[j] = key;
    }
    if (count <= 2) {
        uint64_t sum = 0;
        for (size_t i = 0; i < count; ++i) sum += scratch[i];
        return (uint32_t)(sum / count);
    }
    size_t start = 1;
    size_t end = count - 1;
    uint64_t sum = 0;
    size_t samples = 0;
    for (size_t i = start; i < end; ++i) {
        sum += scratch[i];
        samples++;
    }
    return samples ? (uint32_t)(sum / samples) : scratch[count / 2];
}

static long perf_event_open_sys(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static void enable_perf(perf_ctx_t* ctx) {
    if (!ctx) return;
    if (ctx->fd_cycles >= 0) { ioctl(ctx->fd_cycles, PERF_EVENT_IOC_RESET, 0);  ioctl(ctx->fd_cycles, PERF_EVENT_IOC_ENABLE, 0); }
    if (ctx->fd_insns  >= 0) { ioctl(ctx->fd_insns,  PERF_EVENT_IOC_RESET, 0);  ioctl(ctx->fd_insns,  PERF_EVENT_IOC_ENABLE, 0); }
    if (ctx->fd_llc_misses >= 0) { ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_RESET, 0); ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_ENABLE, 0); }
}

static void disable_perf(perf_ctx_t* ctx) {
    if (!ctx) return;
    if (ctx->fd_cycles >= 0) ioctl(ctx->fd_cycles, PERF_EVENT_IOC_DISABLE, 0);
    if (ctx->fd_insns  >= 0) ioctl(ctx->fd_insns,  PERF_EVENT_IOC_DISABLE, 0);
    if (ctx->fd_llc_misses >= 0) ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_DISABLE, 0);
}

static perf_ctx_t* init_perf_monitoring(void) {
    perf_ctx_t *ctx = calloc(1, sizeof(perf_ctx_t));
    if (!ctx) return NULL;
    ctx->fd_cycles = -1;
    ctx->fd_insns = -1;
    ctx->fd_llc_misses = -1;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    (void)sched_setaffinity(0, sizeof(cpuset), &cpuset);
    ctx->pinned_cpu = 0;

    struct perf_event_attr pe = {0};
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(pe);
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    // CPU cycles (group leader)
    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    long fd_cycles = perf_event_open_sys(&pe, 0, -1, -1, 0);
    if (fd_cycles < 0) { free(ctx); return NULL; }
    ctx->fd_cycles = (int)fd_cycles;

    // Instructions (in same group)
    pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    long fd_insns = perf_event_open_sys(&pe, 0, -1, ctx->fd_cycles, 0);
    if (fd_insns < 0) { close(ctx->fd_cycles); free(ctx); return NULL; }
    ctx->fd_insns = (int)fd_insns;

    // LLC misses (separate counter)
    pe.read_format = 0;
    pe.config = PERF_COUNT_HW_CACHE_MISSES;
    long fd_llc_misses = perf_event_open_sys(&pe, 0, -1, -1, 0);
    if (fd_llc_misses < 0) {
        close(ctx->fd_insns);
        close(ctx->fd_cycles);
        free(ctx);
        return NULL;
    }
    ctx->fd_llc_misses = (int)fd_llc_misses;

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
    struct { uint64_t nr, time_enabled, time_running, values[2]; } rd_before = {0}, rd_after = {0};
    uint64_t llc_before = 0, llc_after = 0;
    ssize_t n = read(ctx->fd_cycles, &rd_before, sizeof(rd_before));
    if (n < 0) { ctx->baseline_cpi = 1000; return; }
    if (ctx->fd_llc_misses >= 0) {
        ssize_t llc_read = read(ctx->fd_llc_misses, &llc_before, sizeof(llc_before));
        if (llc_read < 0) llc_before = 0;
    }
    for (int i = 0; i < 100000; i++) workload_once();
    n = read(ctx->fd_cycles, &rd_after, sizeof(rd_after));
    if (n < 0) { ctx->baseline_cpi = 1000; return; }
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
    printf("Baseline CPI: %lu.%03lu\n", ctx->baseline_cpi / 1000, ctx->baseline_cpi % 1000);
    printf("Baseline MPKI: %lu.%03lu\n", ctx->baseline_llc_mpki_milli / 1000, ctx->baseline_llc_mpki_milli % 1000);
}


static int evaluate_thermal_state(perf_ctx_t *ctx, thermal_eval_t *out) {
    struct { uint64_t nr, time_enabled, time_running, values[2]; } rd_before = {0}, rd_after = {0};
    uint64_t llc_before = 0, llc_after = 0;
    ssize_t n = read(ctx->fd_cycles, &rd_before, sizeof(rd_before));
    if (n < 0) return 0;
    if (ctx->fd_llc_misses >= 0) {
        ssize_t llc_read = read(ctx->fd_llc_misses, &llc_before, sizeof(llc_before));
        if (llc_read < 0) llc_before = 0;
    }
    for (int i = 0; i < 10000; i++) workload_once();
    n = read(ctx->fd_cycles, &rd_after, sizeof(rd_after));
    if (n < 0) return 0;
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
    uint64_t current_cpi = (delta_cycles * 1000) / (delta_insns ?: 1);
    uint64_t delta_llc = (ctx->fd_llc_misses >= 0 && llc_after >= llc_before) ? (llc_after - llc_before) : 0;
    uint64_t mpki_milli = delta_insns ? (delta_llc * MPKI_SCALE) / delta_insns : 0;

    ctx->fast_cpi = update_ewma(ctx->fast_cpi, current_cpi, FAST_EWMA_SHIFT);
    ctx->slow_cpi = update_ewma(ctx->slow_cpi, current_cpi, SLOW_EWMA_SHIFT);
    if (ctx->slow_cpi == 0) ctx->slow_cpi = current_cpi ?: (ctx->baseline_cpi ?: 1000);
    ctx->fast_llc_mpki = update_ewma(ctx->fast_llc_mpki, mpki_milli, FAST_EWMA_SHIFT);
    ctx->slow_llc_mpki = update_ewma(ctx->slow_llc_mpki, mpki_milli, SLOW_EWMA_SHIFT);

    uint64_t reference_cpi = ctx->slow_cpi ?: (ctx->baseline_cpi ?: 1);
    __uint128_t ratio_num = (__uint128_t)current_cpi * 1000u;
    uint64_t ratio_milli = (uint64_t)((ratio_num + reference_cpi / 2) / reference_cpi);
    if (ctx->ratio_history_count < RATIO_HISTORY) {
        ctx->ratio_history_count++;
    }
    uint32_t stored_ratio = (ratio_milli > UINT32_MAX) ? UINT32_MAX : (uint32_t)ratio_milli;
    ctx->ratio_history[ctx->ratio_history_cursor] = stored_ratio;
    ctx->ratio_history_cursor = (ctx->ratio_history_cursor + 1) % RATIO_HISTORY;
    ctx->ratio_trimmed_milli = compute_trimmed_mean(ctx->ratio_history, ctx->ratio_history_count);

    uint64_t baseline_mpki = ctx->baseline_llc_mpki_milli ?: 1000;
    uint64_t mpki_reference = ctx->slow_llc_mpki ?: mpki_milli;
    __uint128_t mpki_ratio_num = (__uint128_t)mpki_reference * 1000u;
    uint64_t mpki_ratio = (uint64_t)((mpki_ratio_num + baseline_mpki / 2) / baseline_mpki);
    int memory_bound = ctx->fd_llc_misses >= 0 && mpki_ratio > 2500; // >2.5x baseline cache pressure.

    uint64_t dynamic_threshold = param_down_ratio_milli;
    if (ctx->fast_cpi > ctx->slow_cpi) {
        uint64_t delta = ctx->fast_cpi - ctx->slow_cpi;
        uint64_t delta_ratio = (uint64_t)(((__uint128_t)delta * 1000u) / (ctx->slow_cpi ?: 1));
        uint64_t slope_penalty = delta_ratio / 5; // respond faster to sharp CPI climbs
        if (slope_penalty > dynamic_threshold / 4) {
            slope_penalty = dynamic_threshold / 4;
        }
        if (dynamic_threshold > slope_penalty) {
            dynamic_threshold -= slope_penalty;
        }
    }
    if (memory_bound) {
        uint64_t guard = dynamic_threshold / 5 + 200; // require more margin if cache bound
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


// ============================================================================
// 5. ADAPTIVE MANAGER
// ============================================================================

static volatile int running = 1;

void* thermal_monitor_thread(void *arg) {
    perf_ctx_t *ctx = (perf_ctx_t*)arg;
    simd_width_t width = __atomic_load_n(&current_width, __ATOMIC_ACQUIRE);
    simd_width_t max_width_cached = detect_max_simd();
    int throttle_count = 0, stable_count = 0, cooldown = 0, dwell_ticks = 0;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET((size_t)ctx->pinned_cpu, &cpuset);
    (void)sched_setaffinity(0, sizeof(cpuset), &cpuset);
    while (running) {
        usleep((useconds_t)param_check_interval_us);
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

int main(int argc, char **argv) {
    printf("=== Production Thermal-Aware SIMD Dispatcher ===\\n\\n");
    parse_flags(argc, argv);
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
    printf("  Cooldown ticks: down=%d up=%d min-dwell=%d\\n",
           cooldown_down_ticks, cooldown_up_ticks, min_dwell_ticks);
    printf("  Demo: %d sec, work iters: %d\\n\\n", demo_duration_sec, work_iters);
    if (init_double_buffer_trampoline() != 0) { fprintf(stderr, "Failed to create trampolines\\n"); return 1; }
    atomic_patch_strict_wx(max_width);
    perf_ctx_t *perf = init_perf_monitoring();
    if (perf) {
        enable_perf(perf);
        measure_baseline_cpi(perf);
        pthread_t monitor; pthread_create(&monitor, NULL, thermal_monitor_thread, perf);
        printf("\\nRunning workload for %d seconds...\\n", demo_duration_sec);
        printf("Try: stress-ng --cpu 8 --cpu-load 100  (to simulate thermal load)\\n\\n");
        for (int sec = 0; sec < demo_duration_sec; sec++) {
            for (int i = 0; i < work_iters; i++) workload_once();
            printf("."); fflush(stdout);
        }
        printf("\\n\\nDone.\\n");
        running = 0;
        pthread_join(monitor, NULL);
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
