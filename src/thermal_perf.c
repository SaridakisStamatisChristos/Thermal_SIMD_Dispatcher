#define _GNU_SOURCE
#include <thermal/simd/thermal_perf.h>

#include <errno.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <thermal/simd/statistics.h>
#include <thermal/simd/telemetry_helper.h>
#include <thermal/simd/logging.h>
#include <thermal/simd/metrics.h>
#include <thermal/simd/thermal_trampoline.h>

#define LOG_COMPONENT "perf"
#define RATIO_HISTORY TSD_RATIO_HISTORY
#define FAST_EWMA_SHIFT 2
#define SLOW_EWMA_SHIFT 5
#define MPKI_SCALE 1000000ULL
#define TSD_TEMP_REF_MILLIC 85000
#define TSD_FREQ_REF_MILLI 1000

typedef struct {
    uint64_t nr;
    uint64_t time_enabled;
    uint64_t time_running;
    uint64_t values[2];
} perf_group_read_t;

struct perf_ctx {
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
    tsd_perf_mode_t mode;
    int software_adaptation;
    struct timespec sw_last_timestamp;
    uint64_t sw_last_iterations;
    tsd_workload_fn workload;
    tsd_telemetry_helper_t telemetry;
    struct timespec mode_entered_at;
    int timeout_notified;
};

_Atomic(uint64_t) g_tsd_workload_iterations = 0;

static int warned_llc_unavailable = 0;
static int warned_perf_group_layout = 0;

static uint64_t timespec_diff_ns(const struct timespec *start, const struct timespec *end);

static int allow_software_upgrades(void) {
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *env = getenv("TSD_ALLOW_SOFTWARE_UPGRADES");
    if (env && env[0] != '\0' && strcmp(env, "0") != 0) {
        cached = 1;
    } else {
        cached = 0;
    }
    return cached;
}

#ifdef TSD_ENABLE_TESTS
#define TSD_PERF_TEST_MAX_STREAMS 4
#define TSD_PERF_TEST_READ_DEFER ((ssize_t)-2)

typedef struct {
    tsd_perf_test_read_stream_t spec;
    size_t step_index;
    size_t data_offset;
} tsd_perf_test_read_stream_state_t;

static tsd_perf_test_read_stream_state_t g_test_read_streams[TSD_PERF_TEST_MAX_STREAMS] = {0};
static size_t g_test_read_stream_count = 0;
#endif

#ifdef TSD_ENABLE_TESTS
typedef ssize_t (*tsd_perf_test_read_hook_t)(int fd, void *buf, size_t count);
static tsd_perf_test_read_hook_t g_test_read_hook = NULL;
#endif

static ssize_t tsd_perf_sys_read(int fd, void *buf, size_t count) {
#ifdef TSD_ENABLE_TESTS
    if (g_test_read_hook) {
        ssize_t rv = g_test_read_hook(fd, buf, count);
        if (rv != TSD_PERF_TEST_READ_DEFER) {
            return rv;
        }
    }
#endif
    return read(fd, buf, count);
}

static void perf_set_mode(perf_ctx_t *ctx, tsd_perf_mode_t mode, const char *reason) {
    if (!ctx) {
        return;
    }
    if (ctx->mode == mode) {
        clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
        return;
    }
    ctx->mode = mode;
    clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
    ctx->timeout_notified = 0;
    const char *why = reason ? reason : "unknown";
    if (mode == TSD_PERF_MODE_SOFTWARE) {
        tsd_metrics_increment(TSD_METRIC_PERF_FALLBACKS);
        tsd_runtime_config_enter_degraded_mode(&g_tsd_config, why);
        tsd_log_warn(LOG_COMPONENT,
                     "event=perf_mode state=software reason=%s pinned_cpu=%d monitor_cpu=%d",
                     why, ctx->pinned_cpu, ctx->monitor_cpu);
        if (!allow_software_upgrades() &&
            atomic_load_explicit(&g_tsd_current_width, memory_order_relaxed) != SIMD_SSE41) {
            if (tsd_trampoline_patch(SIMD_SSE41) == 0) {
                tsd_log_warn(LOG_COMPONENT, "event=perf_mode action=forced-width width=SSE4.1 reason=%s", why);
            }
        }
    } else if (mode == TSD_PERF_MODE_HARDWARE) {
        tsd_metrics_increment(TSD_METRIC_PERF_RECOVERIES);
        tsd_runtime_config_exit_degraded_mode(&g_tsd_config, why);
        tsd_log_info(LOG_COMPONENT,
                     "event=perf_mode state=hardware reason=%s pinned_cpu=%d monitor_cpu=%d",
                     why, ctx->pinned_cpu, ctx->monitor_cpu);
    }
}

static int perf_software_timeout_exceeded(perf_ctx_t *ctx, int timeout_sec) {
    if (!ctx || timeout_sec <= 0) {
        return 0;
    }
    if (ctx->mode != TSD_PERF_MODE_SOFTWARE) {
        return 0;
    }
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t elapsed_ns = timespec_diff_ns(&ctx->mode_entered_at, &now);
    uint64_t limit_ns = (uint64_t)timeout_sec * UINT64_C(1000000000);
    return elapsed_ns >= limit_ns;
}

static int tsd_perf_read_exact(int fd, void *buf, size_t size) {
    if (fd < 0 || !buf || size == 0) {
        return -1;
    }
    uint8_t *out = buf;
    size_t total = 0;
    while (total < size) {
        ssize_t n = tsd_perf_sys_read(fd, out + total, size - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}
#ifdef TSD_ENABLE_TESTS
static ssize_t tsd_perf_test_stream_hook(int fd, void *buf, size_t count) {
    for (size_t i = 0; i < g_test_read_stream_count; ++i) {
        tsd_perf_test_read_stream_state_t *stream = &g_test_read_streams[i];
        if (stream->spec.fd != fd) {
            continue;
        }
        if (stream->step_index >= stream->spec.step_count) {
            return -1;
        }
        const tsd_perf_test_read_step_t *step = &stream->spec.steps[stream->step_index++];
        if (step->type == TSD_PERF_TEST_STEP_EINTR) {
            errno = EINTR;
            return -1;
        }
        size_t remaining = stream->spec.data_len - stream->data_offset;
        size_t to_copy = step->bytes;
        if (to_copy > remaining) {
            to_copy = remaining;
        }
        if (to_copy > count) {
            to_copy = count;
        }
        if (to_copy == 0) {
            return 0;
        }
        memcpy(buf, stream->spec.data + stream->data_offset, to_copy);
        stream->data_offset += to_copy;
        return (ssize_t)to_copy;
    }
    return TSD_PERF_TEST_READ_DEFER;
}

void tsd_perf_test_set_read_streams(const tsd_perf_test_read_stream_t *streams, size_t count) {
    g_test_read_stream_count = (count > TSD_PERF_TEST_MAX_STREAMS) ? TSD_PERF_TEST_MAX_STREAMS : count;
    for (size_t i = 0; i < g_test_read_stream_count; ++i) {
        g_test_read_streams[i].spec = streams[i];
        g_test_read_streams[i].step_index = 0;
        g_test_read_streams[i].data_offset = 0;
    }
    g_test_read_hook = tsd_perf_test_stream_hook;
}

void tsd_perf_test_clear_read_streams(void) {
    g_test_read_stream_count = 0;
    memset(g_test_read_streams, 0, sizeof(g_test_read_streams));
    g_test_read_hook = NULL;
}

perf_ctx_t* tsd_perf_test_create_dummy_context(void) {
    perf_ctx_t *ctx = calloc(1, sizeof(perf_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->fd_cycles = -1;
    ctx->fd_insns = -1;
    ctx->fd_llc_misses = -1;
    ctx->baseline_cpi = 1000;
    ctx->baseline_llc_mpki_milli = 1000;
    ctx->mode = TSD_PERF_MODE_NONE;
    ctx->pinned_cpu = 0;
    ctx->monitor_cpu = 0;
    return ctx;
}

void tsd_perf_test_destroy_dummy_context(perf_ctx_t *ctx) {
    free(ctx);
}

void tsd_perf_test_set_group_fd(perf_ctx_t *ctx, int fd) {
    if (ctx) {
        ctx->fd_cycles = fd;
    }
}

void tsd_perf_test_set_llc_fd(perf_ctx_t *ctx, int fd) {
    if (ctx) {
        ctx->fd_llc_misses = fd;
    }
}

void tsd_perf_test_set_mode(perf_ctx_t *ctx, tsd_perf_mode_t mode) {
    if (ctx) {
        ctx->mode = mode;
        ctx->timeout_notified = 0;
        clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
    }
}

void tsd_test_perf_rewind_mode(perf_ctx_t *ctx, int seconds) {
    if (!ctx) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
    if (seconds > 0) {
        if (ctx->mode_entered_at.tv_sec > seconds) {
            ctx->mode_entered_at.tv_sec -= seconds;
        } else {
            ctx->mode_entered_at.tv_sec = 0;
        }
    }
    ctx->timeout_notified = 0;
}

uint64_t tsd_perf_test_get_baseline_cpi(const perf_ctx_t *ctx) {
    return ctx ? ctx->baseline_cpi : 0;
}

uint64_t tsd_perf_test_get_baseline_mpki(const perf_ctx_t *ctx) {
    return ctx ? ctx->baseline_llc_mpki_milli : 0;
}

int tsd_perf_test_get_last_group_valid(const perf_ctx_t *ctx) {
    return ctx ? ctx->last_group_valid : 0;
}

uint64_t tsd_perf_test_get_last_llc_value(const perf_ctx_t *ctx) {
    return ctx ? ctx->last_llc_value : 0;
}
#endif

#ifdef TSD_ENABLE_TESTS
typedef struct {
    uint32_t ratios[128];
    size_t count;
    size_t index;
    uint32_t mpki;
    int enabled;
    tsd_telemetry_sample_t telemetry[128];
    size_t telemetry_count;
} test_perf_script_t;

static test_perf_script_t g_test_perf_script = {0};
#endif

static long perf_event_open_sys(struct perf_event_attr *hw_event, pid_t pid, int cpu,
                                int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static void try_perf_ioctl(int fd, unsigned long request, const char *what) {
    if (fd < 0) {
        return;
    }
    if (ioctl(fd, request, 0) != 0) {
        char errbuf[128];
        tsd_log_warn(LOG_COMPONENT, "%s: %s", what, tsd_log_strerror(errno, errbuf, sizeof(errbuf)));
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

static inline uint64_t scale_counter(uint64_t delta, uint64_t time_enabled, uint64_t time_running) {
    if (time_running == 0 || time_enabled == 0) {
        return delta;
    }
    __uint128_t num = (__uint128_t)delta * (__uint128_t)time_enabled;
    return (uint64_t)(num / (__uint128_t)time_running);
}

perf_ctx_t* tsd_perf_init(tsd_workload_fn workload_cb) {
    perf_ctx_t *ctx = calloc(1, sizeof(perf_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->fd_cycles = -1;
    ctx->fd_insns = -1;
    ctx->fd_llc_misses = -1;
    ctx->last_group_valid = 0;
    ctx->last_llc_value = 0;
    ctx->mode = TSD_PERF_MODE_NONE;
    ctx->software_adaptation = 0;
    ctx->sw_last_iterations = 0;
    ctx->sw_last_timestamp.tv_sec = 0;
    ctx->sw_last_timestamp.tv_nsec = 0;
    ctx->workload = workload_cb;
    clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
    ctx->timeout_notified = 0;

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

    tsd_telemetry_helper_init(&ctx->telemetry, ctx->monitor_cpu);

    const char *force_sw_env = getenv("TSD_FAKE_PERF");
    int force_sw = force_sw_env && force_sw_env[0] != '\0' && strcmp(force_sw_env, "0") != 0;
    if (force_sw) {
        perf_set_mode(ctx, TSD_PERF_MODE_SOFTWARE, "forced");
        return ctx;
    }

    struct perf_event_attr pe = {0};
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(pe);
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    long fd_cycles = perf_event_open_sys(&pe, 0, ctx->pinned_cpu, -1, 0);
    if (fd_cycles < 0) {
        int err = errno;
        char errbuf[128];
        if (should_fallback_to_software(err)) {
            tsd_log_warn(LOG_COMPONENT,
                         "perf_event_open cycles (falling back to software): %s",
                         tsd_log_strerror(err, errbuf, sizeof(errbuf)));
            perf_set_mode(ctx, TSD_PERF_MODE_SOFTWARE, "cycles-permission");
            return ctx;
        }
        tsd_log_error(LOG_COMPONENT, "perf_event_open cycles: %s",
                      tsd_log_strerror(err, errbuf, sizeof(errbuf)));
        free(ctx);
        return NULL;
    }
    ctx->fd_cycles = (int)fd_cycles;

    pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    long fd_insns = perf_event_open_sys(&pe, 0, ctx->pinned_cpu, ctx->fd_cycles, 0);
    if (fd_insns < 0) {
        int err = errno;
        char errbuf[128];
        close(ctx->fd_cycles);
        ctx->fd_cycles = -1;
        if (should_fallback_to_software(err)) {
            tsd_log_warn(LOG_COMPONENT,
                         "perf_event_open instructions (falling back to software): %s",
                         tsd_log_strerror(err, errbuf, sizeof(errbuf)));
            perf_set_mode(ctx, TSD_PERF_MODE_SOFTWARE, "instructions-permission");
            return ctx;
        }
        tsd_log_error(LOG_COMPONENT, "perf_event_open instructions: %s",
                      tsd_log_strerror(err, errbuf, sizeof(errbuf)));
        free(ctx);
        return NULL;
    }
    ctx->fd_insns = (int)fd_insns;

    pe.read_format = 0;
    pe.config = PERF_COUNT_HW_CACHE_MISSES;
    long fd_llc = perf_event_open_sys(&pe, 0, ctx->pinned_cpu, -1, 0);
    if (fd_llc < 0) {
        int err = errno;
        ctx->fd_llc_misses = -1;
        if (!warned_llc_unavailable) {
            char errbuf[128];
            tsd_log_warn(LOG_COMPONENT,
                         "LLC miss counter unavailable (perf_event_open: %s); memory-bound guard disabled",
                         tsd_log_strerror(err, errbuf, sizeof(errbuf)));
            warned_llc_unavailable = 1;
        }
    } else {
        ctx->fd_llc_misses = (int)fd_llc;
    }

    perf_set_mode(ctx, TSD_PERF_MODE_HARDWARE, "init");
    return ctx;
}

void tsd_perf_enable(perf_ctx_t *ctx) {
    if (!ctx || ctx->mode != TSD_PERF_MODE_HARDWARE) {
        return;
    }
    try_perf_ioctl(ctx->fd_cycles, PERF_EVENT_IOC_RESET, "perf ioctl reset(cycles)");
    try_perf_ioctl(ctx->fd_cycles, PERF_EVENT_IOC_ENABLE, "perf ioctl enable(cycles)");
    try_perf_ioctl(ctx->fd_insns, PERF_EVENT_IOC_RESET, "perf ioctl reset(insns)");
    try_perf_ioctl(ctx->fd_insns, PERF_EVENT_IOC_ENABLE, "perf ioctl enable(insns)");
    try_perf_ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_RESET, "perf ioctl reset(llc)");
    try_perf_ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_ENABLE, "perf ioctl enable(llc)");
}

static void tsd_perf_disable(perf_ctx_t *ctx) {
    if (!ctx || ctx->mode != TSD_PERF_MODE_HARDWARE) {
        return;
    }
    try_perf_ioctl(ctx->fd_cycles, PERF_EVENT_IOC_DISABLE, "perf ioctl disable(cycles)");
    try_perf_ioctl(ctx->fd_insns, PERF_EVENT_IOC_DISABLE, "perf ioctl disable(insns)");
    try_perf_ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_DISABLE, "perf ioctl disable(llc)");
}

void tsd_perf_cleanup(perf_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    tsd_perf_disable(ctx);
    if (ctx->fd_cycles >= 0) {
        close(ctx->fd_cycles);
    }
    if (ctx->fd_insns >= 0) {
        close(ctx->fd_insns);
    }
    if (ctx->fd_llc_misses >= 0) {
        close(ctx->fd_llc_misses);
    }
    tsd_telemetry_helper_destroy(&ctx->telemetry);
    free(ctx);
}

void tsd_perf_measure_baseline(perf_ctx_t *ctx, const tsd_runtime_config *cfg) {
    if (!ctx) {
        return;
    }
    if (ctx->mode == TSD_PERF_MODE_SOFTWARE) {
        struct timespec start = {0}, end = {0};
        const int loops = 100000;
        uint64_t before_iters = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
        if (ctx->workload) {
            clock_gettime(CLOCK_MONOTONIC, &start);
            for (int i = 0; i < loops; i++) {
                ctx->workload();
            }
            clock_gettime(CLOCK_MONOTONIC, &end);
        }
        uint64_t after_iters = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
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
        ctx->ratio_trimmed_milli = cfg ? (uint32_t)cfg->down_ratio_milli : 1500;
        ctx->last_group_valid = 0;
        ctx->last_llc_value = 0;
        ctx->software_adaptation = 1;
        ctx->sw_last_iterations = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
        clock_gettime(CLOCK_MONOTONIC, &ctx->sw_last_timestamp);
        tsd_log_info(LOG_COMPONENT, "Baseline (software) CPI surrogate: %lu.%03lu",
                     surrogate_cpi / 1000, surrogate_cpi % 1000);
        tsd_log_info(LOG_COMPONENT, "Baseline MPKI surrogate: %lu.%03lu",
                     ctx->baseline_llc_mpki_milli / 1000, ctx->baseline_llc_mpki_milli % 1000);
        return;
    }

    perf_group_read_t rd_before = {0}, rd_after = {0};
    uint64_t llc_before = 0, llc_after = 0;
    if (tsd_perf_read_exact(ctx->fd_cycles, &rd_before, sizeof(rd_before)) != 0 || rd_before.nr != 2) {
        ctx->baseline_cpi = 1000;
        return;
    }
    if (ctx->fd_llc_misses >= 0) {
        if (tsd_perf_read_exact(ctx->fd_llc_misses, &llc_before, sizeof(llc_before)) != 0) {
            llc_before = 0;
        }
    }
    if (ctx->workload) {
        for (int i = 0; i < 100000; i++) {
            ctx->workload();
        }
    }
    if (tsd_perf_read_exact(ctx->fd_cycles, &rd_after, sizeof(rd_after)) != 0 || rd_after.nr != 2) {
        ctx->baseline_cpi = 1000;
        return;
    }
    if (ctx->fd_llc_misses >= 0) {
        if (tsd_perf_read_exact(ctx->fd_llc_misses, &llc_after, sizeof(llc_after)) != 0) {
            llc_after = llc_before;
        }
    }
    uint64_t delta_cycles = scale_counter(rd_after.values[0] - rd_before.values[0],
                                          rd_after.time_enabled - rd_before.time_enabled,
                                          rd_after.time_running - rd_before.time_running);
    uint64_t delta_insns = scale_counter(rd_after.values[1] - rd_before.values[1],
                                         rd_after.time_enabled - rd_before.time_enabled,
                                         rd_after.time_running - rd_before.time_running);
    ctx->baseline_cpi = (delta_cycles * 1000) / (delta_insns ?: 1);
    uint64_t delta_llc = (ctx->fd_llc_misses >= 0 && llc_after >= llc_before) ? (llc_after - llc_before) : 0;
    ctx->baseline_llc_mpki_milli = delta_insns ? (delta_llc * MPKI_SCALE) / delta_insns : 0;
    if (ctx->baseline_llc_mpki_milli == 0) {
        ctx->baseline_llc_mpki_milli = 1000;
    }
    ctx->slow_cpi = ctx->fast_cpi = ctx->baseline_cpi ?: 1000;
    ctx->slow_llc_mpki = ctx->fast_llc_mpki = ctx->baseline_llc_mpki_milli;
    ctx->ratio_history_count = 0;
    ctx->ratio_history_cursor = 0;
    ctx->ratio_trimmed_milli = cfg ? (uint32_t)cfg->down_ratio_milli : 1500;
    ctx->last_group_read = rd_after;
    ctx->last_group_valid = 1;
    ctx->last_llc_value = llc_after;
    tsd_log_info(LOG_COMPONENT, "Baseline CPI: %lu.%03lu",
                 ctx->baseline_cpi / 1000, ctx->baseline_cpi % 1000);
    tsd_log_info(LOG_COMPONENT, "Baseline MPKI: %lu.%03lu",
                 ctx->baseline_llc_mpki_milli / 1000, ctx->baseline_llc_mpki_milli % 1000);
    tsd_telemetry_sample_t baseline_sample = {0};
    tsd_telemetry_helper_sample(&ctx->telemetry, &baseline_sample);
    (void)baseline_sample;
}

static int process_measurement(perf_ctx_t *ctx, tsd_thermal_eval_t *out, uint64_t current_cpi,
                               uint64_t mpki_milli, const tsd_runtime_config *cfg,
                               const tsd_telemetry_sample_t *telemetry) {
    if (!ctx) {
        return 0;
    }
    ctx->fast_cpi = tsd_update_ewma(ctx->fast_cpi, current_cpi, FAST_EWMA_SHIFT);
    ctx->slow_cpi = tsd_update_ewma(ctx->slow_cpi, current_cpi, SLOW_EWMA_SHIFT);
    if (ctx->slow_cpi == 0) {
        ctx->slow_cpi = current_cpi ?: (ctx->baseline_cpi ?: 1000);
    }
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

    uint64_t dynamic_threshold = cfg ? cfg->down_ratio_milli : 1500;
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
    if (memory_bound && cfg) {
        uint64_t guard = 0;
        if (cfg->memory_guard_divisor > 0) {
            guard = dynamic_threshold / (uint64_t)cfg->memory_guard_divisor;
        }
        guard += (uint64_t)cfg->memory_guard_offset_milli;
        dynamic_threshold += guard;
    }

    uint64_t consensus_ratio = (ratio_milli + ctx->ratio_trimmed_milli) / 2;
    uint64_t base_severity = (consensus_ratio > dynamic_threshold) ?
                             (consensus_ratio - dynamic_threshold) : 0;

    uint64_t thermal_severity = 0;
    if (cfg && telemetry) {
        if (telemetry->temp_available && cfg->thermal_temp_weight_milli > 0) {
            int64_t temp_excess = (int64_t)telemetry->package_temp_millic - (int64_t)TSD_TEMP_REF_MILLIC;
            if (temp_excess > 0) {
                thermal_severity += ((uint64_t)temp_excess * (uint64_t)cfg->thermal_temp_weight_milli) / 1000ULL;
            }
        }
        if (telemetry->freq_ratio_available && cfg->thermal_ratio_weight_milli > 0) {
            int64_t freq_deficit = (int64_t)TSD_FREQ_REF_MILLI - (int64_t)telemetry->freq_ratio_milli;
            if (freq_deficit > 0) {
                thermal_severity += ((uint64_t)freq_deficit * (uint64_t)cfg->thermal_ratio_weight_milli) / 1000ULL;
            }
        }
    }

    uint64_t severity = base_severity + thermal_severity;

    if (out) {
        out->cpi_milli = current_cpi;
        out->ratio_milli = (uint32_t)ratio_milli;
        out->trimmed_ratio_milli = ctx->ratio_trimmed_milli;
        out->llc_mpki_milli = mpki_milli;
        out->severity_milli = severity;
        out->thermal_severity_milli = thermal_severity;
        out->memory_bound = memory_bound;
        if (telemetry) {
            out->temp_available = telemetry->temp_available;
            out->freq_ratio_available = telemetry->freq_ratio_available;
            out->package_temp_millic = telemetry->package_temp_millic;
            out->freq_ratio_milli = telemetry->freq_ratio_milli;
        } else {
            out->temp_available = 0;
            out->freq_ratio_available = 0;
            out->package_temp_millic = 0;
            out->freq_ratio_milli = 0;
        }
    }
    return severity > 0;
}

int tsd_perf_evaluate(perf_ctx_t *ctx, tsd_thermal_eval_t *out, const tsd_runtime_config *cfg) {
    if (!ctx) {
        return 0;
    }
    if (ctx->mode == TSD_PERF_MODE_SOFTWARE) {
#ifdef TSD_ENABLE_TESTS
        if (g_test_perf_script.enabled && g_test_perf_script.count > 0) {
            size_t script_index = g_test_perf_script.index;
            uint32_t ratio = g_test_perf_script.ratios[script_index];
            if (g_test_perf_script.index + 1 < g_test_perf_script.count) {
                g_test_perf_script.index++;
            }
            uint64_t baseline = ctx->slow_cpi ?: (ctx->baseline_cpi ?: 1000);
            if (baseline == 0) {
                baseline = 1000;
            }
            uint64_t current_cpi = (uint64_t)(((__uint128_t)baseline * ratio + 500u) / 1000u);
            tsd_telemetry_sample_t scripted_telemetry = {0};
            if (g_test_perf_script.telemetry_count > 0) {
                size_t tele_index = script_index;
                if (tele_index >= g_test_perf_script.telemetry_count) {
                    tele_index = g_test_perf_script.telemetry_count - 1;
                }
                scripted_telemetry = g_test_perf_script.telemetry[tele_index];
            } else {
                tsd_telemetry_helper_sample(&ctx->telemetry, &scripted_telemetry);
            }
            return process_measurement(ctx, out, current_cpi, g_test_perf_script.mpki, cfg, &scripted_telemetry);
        }
#endif
        if (!ctx->software_adaptation) {
            ctx->sw_last_iterations = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
            clock_gettime(CLOCK_MONOTONIC, &ctx->sw_last_timestamp);
            ctx->software_adaptation = 1;
            return 0;
        }
        struct timespec now = {0};
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_iters = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
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
        tsd_telemetry_sample_t telemetry = {0};
        tsd_telemetry_helper_sample(&ctx->telemetry, &telemetry);
        return process_measurement(ctx, out, current_cpi, 0, cfg, &telemetry);
    }

    perf_group_read_t rd_now = {0};
    uint64_t llc_now = 0;
    if (tsd_perf_read_exact(ctx->fd_cycles, &rd_now, sizeof(rd_now)) == 0 && rd_now.nr != 2 && !warned_perf_group_layout) {
        tsd_log_warn(LOG_COMPONENT,
                     "perf group returned %" PRIu64 " counters (expected 2); cycle telemetry disabled",
                     (uint64_t)rd_now.nr);
        warned_perf_group_layout = 1;
    }
    if (rd_now.nr != 2) {
        ctx->last_group_valid = 0;
        return 0;
    }
    if (ctx->fd_llc_misses >= 0) {
        if (tsd_perf_read_exact(ctx->fd_llc_misses, &llc_now, sizeof(llc_now)) != 0) {
            llc_now = ctx->last_llc_value;
        }
    }
    if (!ctx->last_group_valid) {
        ctx->last_group_read = rd_now;
        ctx->last_group_valid = 1;
        ctx->last_llc_value = llc_now;
        return 0;
    }

    uint64_t delta_cycles = scale_counter(rd_now.values[0] - ctx->last_group_read.values[0],
                                          rd_now.time_enabled - ctx->last_group_read.time_enabled,
                                          rd_now.time_running - ctx->last_group_read.time_running);
    uint64_t delta_insns = scale_counter(rd_now.values[1] - ctx->last_group_read.values[1],
                                         rd_now.time_enabled - ctx->last_group_read.time_enabled,
                                         rd_now.time_running - ctx->last_group_read.time_running);
    uint64_t current_cpi = (delta_cycles * 1000) / (delta_insns ?: 1);
    uint64_t delta_llc = (ctx->fd_llc_misses >= 0 && llc_now >= ctx->last_llc_value)
                         ? (llc_now - ctx->last_llc_value) : 0;
    uint64_t mpki_milli = delta_insns ? (delta_llc * MPKI_SCALE) / delta_insns : 0;

    ctx->last_group_read = rd_now;
    ctx->last_llc_value = llc_now;

    tsd_telemetry_sample_t telemetry = {0};
    tsd_telemetry_helper_sample(&ctx->telemetry, &telemetry);
    return process_measurement(ctx, out, current_cpi, mpki_milli, cfg, &telemetry);
}

tsd_perf_mode_t tsd_perf_get_mode(const perf_ctx_t *ctx) {
    return ctx ? ctx->mode : TSD_PERF_MODE_NONE;
}

int tsd_perf_get_pinned_cpu(const perf_ctx_t *ctx) {
    return ctx ? ctx->pinned_cpu : -1;
}

int tsd_perf_get_monitor_cpu(const perf_ctx_t *ctx) {
    return ctx ? ctx->monitor_cpu : -1;
}

int tsd_perf_check_software_timeout(perf_ctx_t *ctx, int timeout_sec) {
    if (!ctx || timeout_sec <= 0) {
        return 0;
    }
    if (ctx->timeout_notified) {
        return 0;
    }
    if (!perf_software_timeout_exceeded(ctx, timeout_sec)) {
        return 0;
    }
    ctx->timeout_notified = 1;
    tsd_metrics_increment(TSD_METRIC_SOFTWARE_TIMEOUT_ESCALATIONS);
    tsd_log_error(LOG_COMPONENT,
                  "event=perf_mode state=software action=fail-closed timeout_sec=%d",
                  timeout_sec);
    return 1;
}

#ifdef TSD_ENABLE_TESTS
void tsd_perf_set_fake_script(const uint32_t *ratios, size_t count, uint32_t mpki) {
    if (!ratios || count == 0 || count > sizeof(g_test_perf_script.ratios) / sizeof(g_test_perf_script.ratios[0])) {
        g_test_perf_script.enabled = 0;
        return;
    }
    memcpy(g_test_perf_script.ratios, ratios, count * sizeof(uint32_t));
    g_test_perf_script.count = count;
    g_test_perf_script.index = 0;
    g_test_perf_script.mpki = mpki;
    g_test_perf_script.enabled = 1;
    memset(g_test_perf_script.telemetry, 0, sizeof(g_test_perf_script.telemetry));
    g_test_perf_script.telemetry_count = 0;
}

void tsd_perf_set_fake_telemetry(const tsd_telemetry_sample_t *samples, size_t count) {
    size_t max_entries = sizeof(g_test_perf_script.telemetry) / sizeof(g_test_perf_script.telemetry[0]);
    if (!samples || count == 0) {
        g_test_perf_script.telemetry_count = 0;
        memset(g_test_perf_script.telemetry, 0, sizeof(g_test_perf_script.telemetry));
        return;
    }
    if (count > max_entries) {
        count = max_entries;
    }
    memcpy(g_test_perf_script.telemetry, samples, count * sizeof(tsd_telemetry_sample_t));
    g_test_perf_script.telemetry_count = count;
}

void tsd_perf_clear_fake_script(void) {
    memset(&g_test_perf_script, 0, sizeof(g_test_perf_script));
}
#endif
