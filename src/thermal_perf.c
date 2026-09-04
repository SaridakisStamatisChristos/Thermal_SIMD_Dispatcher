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

#include <observability/telemetry_state.h>
#include <thermal/simd/statistics.h>
#include <thermal/simd/telemetry_helper.h>
#include <thermal/simd/telemetry_fusion.h>
#include <thermal/simd/logging.h>
#include <thermal/simd/metrics.h>
#include <thermal/simd/thermal_trampoline.h>

#include "runtime_guard_internal.h"

#define LOG_COMPONENT "perf"
#define RATIO_HISTORY TSD_RATIO_HISTORY
#define FAST_EWMA_SHIFT 2
#define SLOW_EWMA_SHIFT 5
#define MPKI_SCALE 1000000ULL
#define TSD_TEMP_REF_MILLIC 85000
#define TSD_FREQ_REF_MILLI 1000
#define PERF_INITIAL_BACKOFF_SEC 5
#define PERF_MAX_BACKOFF_SEC 60
#define PERF_RECOVERY_OBSERVE_NS 10000000L
#define PERF_ZERO_RUNNING_LIMIT 3

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
    uint64_t calibrated_cpi_reference;
    uint64_t baseline_work_cost_milli;
    uint64_t calibrated_work_reference;
    uint64_t baseline_llc_mpki_milli;
    uint64_t slow_cpi;
    uint64_t fast_cpi;
    uint64_t slow_llc_mpki;
    uint64_t fast_llc_mpki;
    uint32_t ratio_history[RATIO_HISTORY];
    size_t ratio_history_count;
    size_t ratio_history_cursor;
    uint32_t ratio_trimmed_milli;

    pid_t owner_tid;
    cpu_set_t original_affinity;
    int original_affinity_valid;
    int pinned_cpu;
    int monitor_cpu;

    perf_group_read_t last_group_read;
    int last_group_valid;
    uint64_t last_llc_value;
    int zero_running_samples;

    tsd_perf_mode_t mode;
    int hardware_validated;
    int software_adaptation;
    struct timespec sw_last_timestamp;
    uint64_t sw_last_iterations;
    uint64_t hw_last_iterations;
    tsd_workload_fn workload;
    tsd_telemetry_helper_t telemetry;
    int fusion_acquired;
    struct timespec mode_entered_at;
    int timeout_notified;
    uint64_t perf_retry_deadline_ns;
    int perf_retry_backoff_seconds;
    uint64_t llc_retry_deadline_ns;
    int llc_retry_backoff_seconds;
    int force_software;
};

static int warned_llc_unavailable = 0;
static int warned_perf_group_layout = 0;

static uint64_t timespec_diff_ns(const struct timespec *start, const struct timespec *end);
static void perf_fail_closed(perf_ctx_t *ctx, const char *reason);

static pid_t current_tid(void) {
    return (pid_t)syscall(SYS_gettid);
}

static void reset_measurement_domain(perf_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    ctx->slow_cpi = 0;
    ctx->fast_cpi = 0;
    ctx->slow_llc_mpki = 0;
    ctx->fast_llc_mpki = 0;
    memset(ctx->ratio_history, 0, sizeof(ctx->ratio_history));
    ctx->ratio_history_count = 0;
    ctx->ratio_history_cursor = 0;
    ctx->ratio_trimmed_milli = 0;
}

static void publish_perf_state(const perf_ctx_t *ctx, int healthy) {
    if (!ctx) {
        return;
    }
    tsd_perf_telemetry_t telemetry = {0};
    telemetry.mode = (int)ctx->mode;
    telemetry.counters_healthy = healthy ? 1 : 0;
    telemetry.pinned_cpu = ctx->pinned_cpu;
    telemetry.monitor_cpu = ctx->monitor_cpu;
    tsd_observability_update_perf(&telemetry);
}

static uint64_t monotonic_now_ns(void) {
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void reset_perf_retry(perf_ctx_t *ctx) {
    if (!ctx) return;
    ctx->perf_retry_deadline_ns = 0;
    ctx->perf_retry_backoff_seconds = PERF_INITIAL_BACKOFF_SEC;
}

static void schedule_perf_retry(perf_ctx_t *ctx) {
    if (!ctx || ctx->force_software) return;
    if (ctx->perf_retry_backoff_seconds <= 0) ctx->perf_retry_backoff_seconds = PERF_INITIAL_BACKOFF_SEC;
    int delay = ctx->perf_retry_backoff_seconds;
    if (delay > PERF_MAX_BACKOFF_SEC) delay = PERF_MAX_BACKOFF_SEC;
    uint64_t now = monotonic_now_ns();
    uint64_t delta = (uint64_t)delay * UINT64_C(1000000000);
    ctx->perf_retry_deadline_ns = now > UINT64_MAX - delta ? UINT64_MAX : now + delta;
    if (ctx->perf_retry_backoff_seconds < PERF_MAX_BACKOFF_SEC) {
        int next = ctx->perf_retry_backoff_seconds * 2;
        ctx->perf_retry_backoff_seconds = next > PERF_MAX_BACKOFF_SEC ? PERF_MAX_BACKOFF_SEC : next;
    }
}

static int perf_retry_due(const perf_ctx_t *ctx) {
    if (!ctx || ctx->force_software) return 0;
    return ctx->perf_retry_deadline_ns == 0 || monotonic_now_ns() >= ctx->perf_retry_deadline_ns;
}

static void reset_llc_retry(perf_ctx_t *ctx) {
    if (!ctx) return;
    ctx->llc_retry_deadline_ns = 0;
    ctx->llc_retry_backoff_seconds = PERF_INITIAL_BACKOFF_SEC;
}

static void schedule_llc_retry(perf_ctx_t *ctx) {
    if (!ctx || ctx->force_software) return;
    if (ctx->llc_retry_backoff_seconds <= 0) ctx->llc_retry_backoff_seconds = PERF_INITIAL_BACKOFF_SEC;
    int delay = ctx->llc_retry_backoff_seconds;
    if (delay > PERF_MAX_BACKOFF_SEC) delay = PERF_MAX_BACKOFF_SEC;
    uint64_t now = monotonic_now_ns();
    uint64_t delta = (uint64_t)delay * UINT64_C(1000000000);
    ctx->llc_retry_deadline_ns = now > UINT64_MAX - delta ? UINT64_MAX : now + delta;
    if (ctx->llc_retry_backoff_seconds < PERF_MAX_BACKOFF_SEC) {
        int next = ctx->llc_retry_backoff_seconds * 2;
        ctx->llc_retry_backoff_seconds = next > PERF_MAX_BACKOFF_SEC ? PERF_MAX_BACKOFF_SEC : next;
    }
}

static int llc_retry_due(const perf_ctx_t *ctx) {
    if (!ctx || ctx->force_software || ctx->mode != TSD_PERF_MODE_HARDWARE) return 0;
    return ctx->llc_retry_deadline_ns == 0 || monotonic_now_ns() >= ctx->llc_retry_deadline_ns;
}

static void perf_set_mode(perf_ctx_t *ctx, tsd_perf_mode_t mode, const char *reason) {
    if (!ctx) {
        return;
    }
    tsd_perf_mode_t previous = ctx->mode;
    if (previous == mode) {
        /* Re-publishing the same state must not restart degraded-mode timeout. */
        publish_perf_state(ctx, mode == TSD_PERF_MODE_HARDWARE && ctx->hardware_validated);
        return;
    }

    /* Hardware CPI and software ns/work-item are different physical
     * quantities. Never carry EWMAs/history from one domain into the other. */
    reset_measurement_domain(ctx);
    ctx->mode = mode;
    clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
    ctx->timeout_notified = 0;
    const char *why = reason ? reason : "unknown";

    if (mode == TSD_PERF_MODE_SOFTWARE) {
        /* Revoke admission before changing any physical selector. The guard
         * publication below is authoritative; SSE selection is then cleanup. */
        tsd_runtime_wide_admission_close();
        ctx->calibrated_cpi_reference = 0;
        ctx->calibrated_work_reference = 0;
        ctx->hardware_validated = 0;
        tsd_metrics_increment(TSD_METRIC_PERF_FALLBACKS);
        tsd_runtime_config_enter_degraded_mode(&g_tsd_config, why);
        publish_perf_state(ctx, 0);
        tsd_log_warn(LOG_COMPONENT,
                     "event=perf_mode state=software reason=%s workload_tid=%d pinned_cpu=%d monitor_cpu=%d",
                     why, (int)ctx->owner_tid, ctx->pinned_cpu, ctx->monitor_cpu);
        if (tsd_trampoline_state_current_width() != SIMD_SSE41) {
            if (tsd_trampoline_patch(SIMD_SSE41) == 0) {
                tsd_log_warn(LOG_COMPONENT, "event=perf_mode action=forced-width width=SSE4.1 reason=%s", why);
            } else {
                tsd_log_error(LOG_COMPONENT, "event=perf_mode action=force-width-failed width=SSE4.1 reason=%s errno=%d",
                              why, errno);
            }
        }
    } else if (mode == TSD_PERF_MODE_HARDWARE) {
        if (previous == TSD_PERF_MODE_SOFTWARE && ctx->hardware_validated) {
            tsd_metrics_increment(TSD_METRIC_PERF_RECOVERIES);
        }
        tsd_runtime_config_exit_degraded_mode(&g_tsd_config, why);
        tsd_log_info(LOG_COMPONENT,
                     "event=perf_mode state=hardware validated=%d reason=%s workload_tid=%d pinned_cpu=%d monitor_cpu=%d",
                     ctx->hardware_validated, why, (int)ctx->owner_tid, ctx->pinned_cpu, ctx->monitor_cpu);
        publish_perf_state(ctx, ctx->hardware_validated);
    } else {
        ctx->hardware_validated = 0;
        publish_perf_state(ctx, 0);
    }
}

static int perf_software_timeout_exceeded(perf_ctx_t *ctx, int timeout_sec) {
    if (!ctx || timeout_sec <= 0 || ctx->mode != TSD_PERF_MODE_SOFTWARE) {
        return 0;
    }
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t elapsed_ns = timespec_diff_ns(&ctx->mode_entered_at, &now);
    return elapsed_ns >= (uint64_t)timeout_sec * UINT64_C(1000000000);
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
    g_test_read_stream_count = count > TSD_PERF_TEST_MAX_STREAMS ? TSD_PERF_TEST_MAX_STREAMS : count;
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

static void fetch_fused_telemetry(perf_ctx_t *ctx, tsd_telemetry_sample_t *out) {
    if (!ctx || !out) {
        return;
    }
    if (ctx->fusion_acquired && tsd_telemetry_fusion_sample(out) == 0) {
        return;
    }
    (void)tsd_telemetry_helper_sample(&ctx->telemetry, out);
    if (out->temp_available && !out->filtered_temp_available) {
        out->filtered_temp_available = 1;
        out->filtered_package_temp_millic = out->package_temp_millic;
    }
    if (out->freq_ratio_available && !out->filtered_freq_ratio_available) {
        out->filtered_freq_ratio_available = 1;
        out->filtered_freq_ratio_milli = out->freq_ratio_milli;
    }
}

static int perf_ioctl(int fd, unsigned long request, unsigned long arg, const char *what, int required) {
    if (fd < 0) {
        return required ? -1 : 0;
    }
    if (ioctl(fd, request, arg) != 0) {
        char errbuf[128];
        tsd_log_warn(LOG_COMPONENT, "%s: %s", what, tsd_log_strerror(errno, errbuf, sizeof(errbuf)));
        return -1;
    }
    return 0;
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
    return (uint64_t)sec * UINT64_C(1000000000) + (uint64_t)nsec;
}

static inline uint64_t scale_counter(uint64_t delta, uint64_t time_enabled, uint64_t time_running) {
    if (time_running == 0 || time_enabled == 0) {
        return 0;
    }
    __uint128_t num = (__uint128_t)delta * (__uint128_t)time_enabled;
    return (uint64_t)(num / (__uint128_t)time_running);
}

static int group_order_valid(const perf_group_read_t *before, const perf_group_read_t *after) {
    if (!before || !after || before->nr != 2 || after->nr != 2) {
        return 0;
    }
    if (after->time_enabled < before->time_enabled || after->time_running < before->time_running) {
        return 0;
    }
    if (after->values[0] < before->values[0] || after->values[1] < before->values[1]) {
        return 0;
    }
    return 1;
}

static int group_baseline_progress_valid(const perf_group_read_t *before,
                                         const perf_group_read_t *after) {
    if (!group_order_valid(before, after)) {
        return 0;
    }
    return after->time_enabled > before->time_enabled &&
           after->time_running > before->time_running &&
           after->values[0] > before->values[0] &&
           after->values[1] > before->values[1];
}

static void close_perf_events(perf_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->fd_cycles >= 0) {
        close(ctx->fd_cycles);
    }
    if (ctx->fd_insns >= 0) {
        close(ctx->fd_insns);
    }
    if (ctx->fd_llc_misses >= 0) {
        close(ctx->fd_llc_misses);
    }
    ctx->fd_cycles = -1;
    ctx->fd_insns = -1;
    ctx->fd_llc_misses = -1;
    ctx->last_group_valid = 0;
    ctx->last_llc_value = 0;
    ctx->zero_running_samples = 0;
}

static void select_runtime_cpus(perf_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    ctx->owner_tid = current_tid();
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    int first = -1;
    int second = -1;

    if (sched_getaffinity(ctx->owner_tid, sizeof(allowed), &allowed) == 0) {
        ctx->original_affinity = allowed;
        ctx->original_affinity_valid = 1;
        for (size_t cpu = 0; cpu < (size_t)CPU_SETSIZE; ++cpu) {
            if (!CPU_ISSET(cpu, &allowed)) {
                continue;
            }
            if (first < 0) {
                first = (int)cpu;
            } else {
                second = (int)cpu;
                break;
            }
        }
    }

    if (first < 0) {
        int current = sched_getcpu();
        first = current >= 0 ? current : 0;
        second = first;
        tsd_log_warn(LOG_COMPONENT, "sched_getaffinity failed; using current cpu=%d", first);
    }
    if (second < 0) {
        second = first;
    }

    ctx->pinned_cpu = first;
    ctx->monitor_cpu = second;

    cpu_set_t pin;
    CPU_ZERO(&pin);
    CPU_SET((size_t)first, &pin);
    if (sched_setaffinity(ctx->owner_tid, sizeof(pin), &pin) != 0) {
        char errbuf[128];
        tsd_log_warn(LOG_COMPONENT, "failed to pin workload tid=%d cpu=%d: %s", (int)ctx->owner_tid, first,
                     tsd_log_strerror(errno, errbuf, sizeof(errbuf)));
    }
}

static void restore_owner_affinity(perf_ctx_t *ctx) {
    if (!ctx || !ctx->original_affinity_valid || ctx->owner_tid <= 0) {
        return;
    }
    if (sched_setaffinity(ctx->owner_tid, sizeof(ctx->original_affinity), &ctx->original_affinity) != 0 && errno != ESRCH) {
        char errbuf[128];
        tsd_log_warn(LOG_COMPONENT, "failed to restore workload tid=%d affinity: %s",
                     (int)ctx->owner_tid, tsd_log_strerror(errno, errbuf, sizeof(errbuf)));
    }
    ctx->original_affinity_valid = 0;
}

static int open_llc_event(perf_ctx_t *ctx) {
    if (!ctx) {
        errno = EINVAL;
        return -1;
    }
    if (ctx->fd_llc_misses >= 0) {
        close(ctx->fd_llc_misses);
        ctx->fd_llc_misses = -1;
    }

    struct perf_event_attr pe = {0};
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(pe);
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.config = PERF_COUNT_HW_CACHE_MISSES;

    long fd_llc = perf_event_open_sys(&pe, ctx->owner_tid, ctx->pinned_cpu, -1, 0);
    if (fd_llc < 0) {
        schedule_llc_retry(ctx);
        return -1;
    }
    ctx->fd_llc_misses = (int)fd_llc;
    reset_llc_retry(ctx);
    return 0;
}

static int open_perf_events(perf_ctx_t *ctx) {
    if (!ctx) {
        errno = EINVAL;
        return -1;
    }
    close_perf_events(ctx);

    struct perf_event_attr pe = {0};
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(pe);
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    long fd_cycles = perf_event_open_sys(&pe, ctx->owner_tid, ctx->pinned_cpu, -1, 0);
    if (fd_cycles < 0) {
        return -1;
    }
    ctx->fd_cycles = (int)fd_cycles;

    pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    long fd_insns = perf_event_open_sys(&pe, ctx->owner_tid, ctx->pinned_cpu, ctx->fd_cycles, 0);
    if (fd_insns < 0) {
        int err = errno;
        close_perf_events(ctx);
        errno = err;
        return -1;
    }
    ctx->fd_insns = (int)fd_insns;

    if (open_llc_event(ctx) != 0 && !warned_llc_unavailable) {
        int err = errno;
        char errbuf[128];
        tsd_log_warn(LOG_COMPONENT,
                     "LLC miss counter unavailable (perf_event_open: %s); memory-bound guard will retry independently",
                     tsd_log_strerror(err, errbuf, sizeof(errbuf)));
        warned_llc_unavailable = 1;
    }
    reset_perf_retry(ctx);
    return 0;
}

static int enable_optional_llc(perf_ctx_t *ctx, const char *phase) {
    if (!ctx || ctx->fd_llc_misses < 0) {
        return 0;
    }
    if (perf_ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_RESET, 0, "perf ioctl reset(llc)", 0) != 0 ||
        perf_ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_ENABLE, 0, "perf ioctl enable(llc)", 0) != 0) {
        tsd_log_warn(LOG_COMPONENT, "event=llc_counter state=degraded phase=%s", phase ? phase : "unknown");
        close(ctx->fd_llc_misses);
        ctx->fd_llc_misses = -1;
        schedule_llc_retry(ctx);
        return -1;
    }
    return 0;
}

static int enable_perf_events_strict(perf_ctx_t *ctx, const char *phase) {
    if (!ctx || ctx->fd_cycles < 0 || ctx->fd_insns < 0) {
        errno = EBADF;
        return -1;
    }
    if (perf_ioctl(ctx->fd_cycles, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP,
                   "perf ioctl reset(primary group)", 1) != 0 ||
        perf_ioctl(ctx->fd_cycles, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP,
                   "perf ioctl enable(primary group)", 1) != 0) {
        tsd_log_error(LOG_COMPONENT, "event=perf_group state=enable-failed phase=%s", phase ? phase : "unknown");
        return -1;
    }
    (void)enable_optional_llc(ctx, phase);
    return 0;
}

static void perf_fail_closed(perf_ctx_t *ctx, const char *reason) {
    if (!ctx) {
        return;
    }
    close_perf_events(ctx);
    ctx->hardware_validated = 0;
    ctx->software_adaptation = 0;
    schedule_perf_retry(ctx);
    perf_set_mode(ctx, TSD_PERF_MODE_SOFTWARE, reason ? reason : "counter-loss");
}

perf_ctx_t* tsd_perf_init(tsd_workload_fn workload_cb) {
    perf_ctx_t *ctx = calloc(1, sizeof(perf_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->fd_cycles = -1;
    ctx->fd_insns = -1;
    ctx->fd_llc_misses = -1;
    ctx->mode = TSD_PERF_MODE_NONE;
    ctx->workload = workload_cb;
    reset_perf_retry(ctx);
    reset_llc_retry(ctx);
    clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
    select_runtime_cpus(ctx);
    publish_perf_state(ctx, 0);

    (void)tsd_telemetry_helper_init(&ctx->telemetry, ctx->pinned_cpu);
    ctx->fusion_acquired = tsd_telemetry_fusion_start_for_cpu(ctx->pinned_cpu) == 0;
    if (!ctx->fusion_acquired) {
        tsd_log_warn(LOG_COMPONENT,
                     "process-wide fusion already targets another CPU; using cpu-local direct telemetry for cpu=%d",
                     ctx->pinned_cpu);
    }

    const char *force_sw_env = getenv("TSD_FAKE_PERF");
    ctx->force_software = force_sw_env && force_sw_env[0] != '\0' && strcmp(force_sw_env, "0") != 0;
    if (ctx->force_software) {
        perf_set_mode(ctx, TSD_PERF_MODE_SOFTWARE, "forced");
        return ctx;
    }

    if (open_perf_events(ctx) != 0) {
        int err = errno;
        char errbuf[128];
        if (should_fallback_to_software(err)) {
            tsd_log_warn(LOG_COMPONENT, "perf_event_open unavailable; falling back to software: %s",
                         tsd_log_strerror(err, errbuf, sizeof(errbuf)));
        } else {
            tsd_log_error(LOG_COMPONENT, "perf_event_open failed; entering recoverable software mode: %s",
                          tsd_log_strerror(err, errbuf, sizeof(errbuf)));
        }
        schedule_perf_retry(ctx);
        perf_set_mode(ctx, TSD_PERF_MODE_SOFTWARE, "perf-open");
        return ctx;
    }

    /* Open is not proof of counter health; baseline validation completes it. */
    ctx->hardware_validated = 0;
    perf_set_mode(ctx, TSD_PERF_MODE_HARDWARE, "init-open");
    return ctx;
}

void tsd_perf_enable(perf_ctx_t *ctx) {
    if (!ctx || ctx->mode != TSD_PERF_MODE_HARDWARE) {
        return;
    }
    if (enable_perf_events_strict(ctx, "explicit-enable") != 0) {
        perf_fail_closed(ctx, "enable");
    }
}

static void tsd_perf_disable(perf_ctx_t *ctx) {
    if (!ctx || ctx->mode != TSD_PERF_MODE_HARDWARE) {
        return;
    }
    (void)perf_ioctl(ctx->fd_cycles, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP,
                     "perf ioctl disable(primary group)", 0);
    (void)perf_ioctl(ctx->fd_llc_misses, PERF_EVENT_IOC_DISABLE, 0,
                     "perf ioctl disable(llc)", 0);
}

void tsd_perf_cleanup(perf_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    tsd_perf_disable(ctx);
    close_perf_events(ctx);
    tsd_telemetry_helper_destroy(&ctx->telemetry);
    if (ctx->fusion_acquired) {
        tsd_telemetry_fusion_stop();
    }
    ctx->mode = TSD_PERF_MODE_NONE;
    ctx->hardware_validated = 0;
    publish_perf_state(ctx, 0);
    restore_owner_affinity(ctx);
    free(ctx);
}

static void local_probe_work(void) {
    volatile uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    for (int i = 0; i < 200000; ++i) {
        state ^= state << 7;
        state ^= state >> 9;
        state *= UINT64_C(0xbf58476d1ce4e5b9);
    }
    (void)state;
}

static void create_baseline_observation(perf_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    if (current_tid() == ctx->owner_tid) {
        if (ctx->workload) {
            for (int i = 0; i < 100000; ++i) {
                ctx->workload();
            }
        } else {
            local_probe_work();
        }
        return;
    }

    /*
     * Reprobes execute on the monitor thread while perf events remain bound to
     * the original workload TID. Give that thread a bounded observation window
     * rather than accidentally benchmarking the monitor itself.
     */
    struct timespec wait = {.tv_sec = 0, .tv_nsec = PERF_RECOVERY_OBSERVE_NS};
    while (nanosleep(&wait, &wait) != 0 && errno == EINTR) {}
}

static int measure_hardware_baseline(perf_ctx_t *ctx, const tsd_runtime_config *cfg) {
    perf_group_read_t rd_before = {0}, rd_after = {0};
    uint64_t llc_before = 0, llc_after = 0;

    if (!ctx || tsd_perf_read_exact(ctx->fd_cycles, &rd_before, sizeof(rd_before)) != 0 || rd_before.nr != 2) {
        return -1;
    }
    if (ctx->fd_llc_misses >= 0 && tsd_perf_read_exact(ctx->fd_llc_misses, &llc_before, sizeof(llc_before)) != 0) {
        llc_before = 0;
    }

    uint64_t work_before = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
    create_baseline_observation(ctx);
    uint64_t work_after = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);

    if (tsd_perf_read_exact(ctx->fd_cycles, &rd_after, sizeof(rd_after)) != 0 || rd_after.nr != 2) {
        return -1;
    }
    if (!group_baseline_progress_valid(&rd_before, &rd_after)) {
        tsd_log_warn(LOG_COMPONENT,
                     "event=perf_group state=not-running enabled_delta=%" PRIu64 " running_delta=%" PRIu64
                     " cycles_delta=%" PRIu64 " insns_delta=%" PRIu64,
                     rd_after.time_enabled - rd_before.time_enabled,
                     rd_after.time_running - rd_before.time_running,
                     rd_after.values[0] - rd_before.values[0],
                     rd_after.values[1] - rd_before.values[1]);
        return -1;
    }

    if (ctx->fd_llc_misses >= 0 && tsd_perf_read_exact(ctx->fd_llc_misses, &llc_after, sizeof(llc_after)) != 0) {
        llc_after = llc_before;
    }

    uint64_t enabled_delta = rd_after.time_enabled - rd_before.time_enabled;
    uint64_t running_delta = rd_after.time_running - rd_before.time_running;
    uint64_t delta_cycles = scale_counter(rd_after.values[0] - rd_before.values[0], enabled_delta, running_delta);
    uint64_t delta_insns = scale_counter(rd_after.values[1] - rd_before.values[1], enabled_delta, running_delta);
    if (delta_cycles == 0 || delta_insns == 0) {
        return -1;
    }

    ctx->baseline_cpi = (delta_cycles * 1000) / delta_insns;
    ctx->calibrated_cpi_reference = ctx->baseline_cpi ? ctx->baseline_cpi : 1;
    uint64_t baseline_work = work_after >= work_before ? work_after - work_before : 0;
    ctx->baseline_work_cost_milli = baseline_work
        ? (uint64_t)(((__uint128_t)delta_cycles * 1000u) / baseline_work)
        : 0;
    ctx->calibrated_work_reference = ctx->baseline_work_cost_milli;
    ctx->hw_last_iterations = work_after;
    uint64_t delta_llc = (ctx->fd_llc_misses >= 0 && llc_after >= llc_before) ? (llc_after - llc_before) : 0;
    ctx->baseline_llc_mpki_milli = (delta_llc * MPKI_SCALE) / delta_insns;
    if (ctx->baseline_llc_mpki_milli == 0) {
        ctx->baseline_llc_mpki_milli = 1000;
    }
    ctx->slow_cpi = ctx->fast_cpi = ctx->baseline_cpi ? ctx->baseline_cpi : 1000;
    ctx->slow_llc_mpki = ctx->fast_llc_mpki = ctx->baseline_llc_mpki_milli;
    ctx->ratio_history_count = 0;
    ctx->ratio_history_cursor = 0;
    ctx->ratio_trimmed_milli = cfg ? (uint32_t)cfg->down_ratio_milli : 1500;
    ctx->last_group_read = rd_after;
    ctx->last_group_valid = 1;
    ctx->last_llc_value = llc_after;
    ctx->zero_running_samples = 0;

    tsd_log_info(LOG_COMPONENT, "Baseline CPI: %lu.%03lu",
                 ctx->baseline_cpi / 1000, ctx->baseline_cpi % 1000);
    tsd_log_info(LOG_COMPONENT, "Baseline MPKI: %lu.%03lu",
                 ctx->baseline_llc_mpki_milli / 1000, ctx->baseline_llc_mpki_milli % 1000);
    tsd_telemetry_sample_t baseline_sample = {0};
    fetch_fused_telemetry(ctx, &baseline_sample);
    return 0;
}

void tsd_perf_measure_baseline(perf_ctx_t *ctx, const tsd_runtime_config *cfg) {
    if (!ctx) {
        return;
    }

    if (ctx->mode == TSD_PERF_MODE_SOFTWARE) {
        struct timespec start = {0}, end = {0};
        const int loops = 100000;
        uint64_t before_iters = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
        clock_gettime(CLOCK_MONOTONIC, &start);
        if (ctx->workload) {
            for (int i = 0; i < loops; i++) {
                ctx->workload();
            }
        } else {
            local_probe_work();
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        uint64_t after_iters = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
        uint64_t delta_iters = after_iters > before_iters ? after_iters - before_iters : (uint64_t)loops;
        uint64_t elapsed_ns = timespec_diff_ns(&start, &end);
        if (elapsed_ns == 0) {
            elapsed_ns = 1;
        }
        uint64_t surrogate_cpi = delta_iters == 0 ? 1000 : ((elapsed_ns * 1000ULL) / delta_iters);
        if (surrogate_cpi == 0) {
            surrogate_cpi = 1000;
        }
        ctx->baseline_cpi = surrogate_cpi;
        ctx->calibrated_cpi_reference = surrogate_cpi;
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
        return;
    }

    if (ctx->mode != TSD_PERF_MODE_HARDWARE) {
        return;
    }
    if (enable_perf_events_strict(ctx, "baseline") != 0 || measure_hardware_baseline(ctx, cfg) != 0) {
        perf_fail_closed(ctx, "baseline-validation");
        return;
    }
    ctx->hardware_validated = 1;
    publish_perf_state(ctx, 1);
}

static int try_recover_hardware(perf_ctx_t *ctx) {
    if (!ctx || ctx->mode != TSD_PERF_MODE_SOFTWARE || !perf_retry_due(ctx)) {
        return 0;
    }
    if (open_perf_events(ctx) != 0) {
        int err = errno;
        char errbuf[128];
        tsd_log_debug(LOG_COMPONENT, "event=perf_reprobe state=pending errno=%d detail=%s",
                      err, tsd_log_strerror(err, errbuf, sizeof(errbuf)));
        schedule_perf_retry(ctx);
        publish_perf_state(ctx, 0);
        return 0;
    }

    if (enable_perf_events_strict(ctx, "reprobe") != 0 ||
        measure_hardware_baseline(ctx, tsd_runtime_config_active_snapshot()) != 0) {
        tsd_log_warn(LOG_COMPONENT, "event=perf_reprobe state=rejected reason=validation");
        close_perf_events(ctx);
        ctx->hardware_validated = 0;
        schedule_perf_retry(ctx);
        publish_perf_state(ctx, 0);
        return 0;
    }

    ctx->hardware_validated = 1;
    ctx->software_adaptation = 0;
    reset_perf_retry(ctx);
    perf_set_mode(ctx, TSD_PERF_MODE_HARDWARE, "reprobe");
    return 1;
}

static void try_recover_llc(perf_ctx_t *ctx) {
    if (!ctx || ctx->mode != TSD_PERF_MODE_HARDWARE || ctx->fd_llc_misses >= 0 || !llc_retry_due(ctx)) {
        return;
    }
    if (open_llc_event(ctx) != 0) {
        return;
    }
    if (enable_optional_llc(ctx, "reprobe") != 0) {
        return;
    }
    uint64_t seed = 0;
    if (tsd_perf_read_exact(ctx->fd_llc_misses, &seed, sizeof(seed)) != 0) {
        close(ctx->fd_llc_misses);
        ctx->fd_llc_misses = -1;
        schedule_llc_retry(ctx);
        return;
    }
    ctx->last_llc_value = seed;
    reset_llc_retry(ctx);
    tsd_log_info(LOG_COMPONENT, "event=llc_counter state=recovered workload_tid=%d cpu=%d",
                 (int)ctx->owner_tid, ctx->pinned_cpu);
}

static int process_measurement(perf_ctx_t *ctx, tsd_thermal_eval_t *out, uint64_t current_cpi,
                               uint64_t current_work_cost_milli, uint64_t mpki_milli,
                               const tsd_runtime_config *cfg,
                               const tsd_telemetry_sample_t *telemetry) {
    if (!ctx) {
        return 0;
    }
    if (ctx->calibrated_cpi_reference == 0 && current_cpi > 0) {
        ctx->calibrated_cpi_reference = current_cpi;
    }
    ctx->fast_cpi = tsd_update_ewma(ctx->fast_cpi, current_cpi, FAST_EWMA_SHIFT);
    ctx->slow_cpi = tsd_update_ewma(ctx->slow_cpi, current_cpi, SLOW_EWMA_SHIFT);
    if (ctx->slow_cpi == 0) {
        ctx->slow_cpi = current_cpi ? current_cpi : (ctx->baseline_cpi ? ctx->baseline_cpi : 1000);
    }
    ctx->fast_llc_mpki = tsd_update_ewma(ctx->fast_llc_mpki, mpki_milli, FAST_EWMA_SHIFT);
    ctx->slow_llc_mpki = tsd_update_ewma(ctx->slow_llc_mpki, mpki_milli, SLOW_EWMA_SHIFT);

    uint64_t reference_cpi = ctx->slow_cpi ? ctx->slow_cpi : (ctx->baseline_cpi ? ctx->baseline_cpi : 1);
    __uint128_t cpi_ratio_num = (__uint128_t)current_cpi * 1000u;
    uint64_t adaptive_ratio_milli = (uint64_t)((cpi_ratio_num + reference_cpi / 2) / reference_cpi);
    uint64_t absolute_reference = ctx->calibrated_cpi_reference ? ctx->calibrated_cpi_reference : reference_cpi;
    uint64_t absolute_ratio_milli = (uint64_t)((cpi_ratio_num + absolute_reference / 2) / absolute_reference);
    uint64_t cpi_ratio_milli = adaptive_ratio_milli > absolute_ratio_milli
                                   ? adaptive_ratio_milli : absolute_ratio_milli;

    uint64_t ratio_milli = cpi_ratio_milli;
    if (current_work_cost_milli > 0) {
        if (ctx->calibrated_work_reference == 0) {
            ctx->calibrated_work_reference = current_work_cost_milli;
            ctx->baseline_work_cost_milli = current_work_cost_milli;
        }
        __uint128_t work_ratio_num = (__uint128_t)current_work_cost_milli * 1000u;
        ratio_milli = (uint64_t)((work_ratio_num + ctx->calibrated_work_reference / 2) /
                                 ctx->calibrated_work_reference);
    }
    if (ctx->ratio_history_count < RATIO_HISTORY) {
        ctx->ratio_history_count++;
    }
    uint32_t stored_ratio = ratio_milli > UINT32_MAX ? UINT32_MAX : (uint32_t)ratio_milli;
    ctx->ratio_history[ctx->ratio_history_cursor] = stored_ratio;
    ctx->ratio_history_cursor = (ctx->ratio_history_cursor + 1) % RATIO_HISTORY;
    ctx->ratio_trimmed_milli = tsd_compute_trimmed_mean(ctx->ratio_history, ctx->ratio_history_count);

    uint64_t baseline_mpki = ctx->baseline_llc_mpki_milli ? ctx->baseline_llc_mpki_milli : 1000;
    uint64_t mpki_reference = ctx->slow_llc_mpki ? ctx->slow_llc_mpki : mpki_milli;
    __uint128_t mpki_ratio_num = (__uint128_t)mpki_reference * 1000u;
    uint64_t mpki_ratio = (uint64_t)((mpki_ratio_num + baseline_mpki / 2) / baseline_mpki);
    int memory_bound = ctx->fd_llc_misses >= 0 && mpki_ratio > 2500;

    uint64_t dynamic_threshold = cfg ? tsd_runtime_config_effective_down_ratio_milli(cfg) : 1500;
    if (ctx->fast_cpi > ctx->slow_cpi) {
        uint64_t delta = ctx->fast_cpi - ctx->slow_cpi;
        uint64_t delta_ratio = (uint64_t)(((__uint128_t)delta * 1000u) / (ctx->slow_cpi ? ctx->slow_cpi : 1));
        uint64_t slope_penalty = delta_ratio / 5;
        if (slope_penalty > dynamic_threshold / 4) {
            slope_penalty = dynamic_threshold / 4;
        }
        if (dynamic_threshold > slope_penalty) {
            dynamic_threshold -= slope_penalty;
        }
    }
    if (memory_bound && cfg) {
        uint64_t guard = cfg->memory_guard_divisor > 0
                             ? dynamic_threshold / (uint64_t)cfg->memory_guard_divisor
                             : 0;
        guard += (uint64_t)cfg->memory_guard_offset_milli;
        dynamic_threshold += guard;
    }

    uint64_t consensus_ratio = (ratio_milli + ctx->ratio_trimmed_milli) / 2;
    uint64_t base_severity = consensus_ratio > dynamic_threshold ? consensus_ratio - dynamic_threshold : 0;
    uint64_t thermal_severity = 0;
    if (cfg && telemetry) {
        /* Reactive safety always uses raw telemetry, never the filtered channel. */
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
        out->performance_available = 1;
        out->work_normalized = current_work_cost_milli > 0 ? 1 : 0;
        out->work_cost_milli = current_work_cost_milli;
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
            out->filtered_temp_available = telemetry->filtered_temp_available;
            out->filtered_freq_ratio_available = telemetry->filtered_freq_ratio_available;
            out->filtered_package_temp_millic = telemetry->filtered_package_temp_millic;
            out->filtered_freq_ratio_milli = telemetry->filtered_freq_ratio_milli;
        }
    }
    return severity > 0;
}

static int handle_no_running_progress(perf_ctx_t *ctx, tsd_thermal_eval_t *out) {
    if (!ctx) {
        return 0;
    }
    ctx->zero_running_samples++;
    if (ctx->zero_running_samples < PERF_ZERO_RUNNING_LIMIT) {
        return 0;
    }
    tsd_log_error(LOG_COMPONENT, "perf group stopped accumulating runtime; entering software degraded mode");
    perf_fail_closed(ctx, "group-not-running");
    if (out) {
        out->severity_milli = 1;
    }
    return 1;
}

int tsd_perf_evaluate(perf_ctx_t *ctx, tsd_thermal_eval_t *out, const tsd_runtime_config *cfg) {
    if (!ctx) {
        return 0;
    }
    if (out) {
        memset(out, 0, sizeof(*out));
    }

#ifdef TSD_ENABLE_TESTS
    if (g_test_perf_script.enabled && g_test_perf_script.count > 0) {
        size_t script_index = g_test_perf_script.index;
        uint32_t ratio = g_test_perf_script.ratios[script_index];
        if (g_test_perf_script.index + 1 < g_test_perf_script.count) {
            g_test_perf_script.index++;
        }
        uint64_t baseline = ctx->slow_cpi ? ctx->slow_cpi : (ctx->baseline_cpi ? ctx->baseline_cpi : 1000);
        uint64_t current_cpi = (uint64_t)(((__uint128_t)baseline * ratio + 500u) / 1000u);
        tsd_telemetry_sample_t scripted_telemetry = {0};
        if (g_test_perf_script.telemetry_count > 0) {
            size_t tele_index = script_index < g_test_perf_script.telemetry_count
                                    ? script_index
                                    : g_test_perf_script.telemetry_count - 1;
            scripted_telemetry = g_test_perf_script.telemetry[tele_index];
            if (scripted_telemetry.temp_available && !scripted_telemetry.filtered_temp_available) {
                scripted_telemetry.filtered_temp_available = 1;
                scripted_telemetry.filtered_package_temp_millic = scripted_telemetry.package_temp_millic;
            }
            if (scripted_telemetry.freq_ratio_available && !scripted_telemetry.filtered_freq_ratio_available) {
                scripted_telemetry.filtered_freq_ratio_available = 1;
                scripted_telemetry.filtered_freq_ratio_milli = scripted_telemetry.freq_ratio_milli;
            }
        } else {
            fetch_fused_telemetry(ctx, &scripted_telemetry);
        }
        return process_measurement(ctx, out, current_cpi, 0, g_test_perf_script.mpki, cfg, &scripted_telemetry);
    }
#endif
    if (ctx->mode == TSD_PERF_MODE_SOFTWARE) {
        if (try_recover_hardware(ctx)) {
            return 0;
        }
        if (!ctx->software_adaptation) {
            ctx->sw_last_iterations = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
            clock_gettime(CLOCK_MONOTONIC, &ctx->sw_last_timestamp);
            ctx->software_adaptation = 1;
            return 0;
        }
        struct timespec now = {0};
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_iters = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
        uint64_t delta_iters = now_iters >= ctx->sw_last_iterations ? now_iters - ctx->sw_last_iterations : 0;
        uint64_t delta_ns = timespec_diff_ns(&ctx->sw_last_timestamp, &now);
        ctx->sw_last_iterations = now_iters;
        ctx->sw_last_timestamp = now;
        if (delta_iters == 0 || delta_ns == 0) {
            return 0;
        }
        /* Software mode estimates elapsed nanoseconds per completed work
         * item, scaled by 1000. It is a control cost, not hardware CPI. The
         * domain reset on mode entry ensures ratios are only compared against
         * software-mode history. */
        uint64_t software_cost_milli = (delta_ns * 1000ULL) / delta_iters;
        if (software_cost_milli == 0) {
            software_cost_milli = 1;
        }
        tsd_telemetry_sample_t telemetry = {0};
        fetch_fused_telemetry(ctx, &telemetry);
        int software_rc = process_measurement(ctx, out, software_cost_milli, 0, 0, cfg, &telemetry);
        /* Software mode is a fail-closed observability domain, never an
         * authority for normal width optimization or predictive history. */
        if (out) out->performance_available = 0;
        return software_rc;
    }

    if (ctx->mode != TSD_PERF_MODE_HARDWARE || !ctx->hardware_validated) {
        if (out) {
            out->severity_milli = 1;
        }
        return 1;
    }

    try_recover_llc(ctx);

    perf_group_read_t rd_now = {0};
    uint64_t llc_now = 0;
    if (tsd_perf_read_exact(ctx->fd_cycles, &rd_now, sizeof(rd_now)) != 0) {
        tsd_log_error(LOG_COMPONENT, "perf group read failed; entering software degraded mode");
        perf_fail_closed(ctx, "group-read");
        if (out) {
            out->severity_milli = 1;
        }
        return 1;
    }
    if (rd_now.nr != 2) {
        if (!warned_perf_group_layout) {
            tsd_log_warn(LOG_COMPONENT,
                         "perf group returned %" PRIu64 " counters (expected 2); entering software degraded mode",
                         (uint64_t)rd_now.nr);
            warned_perf_group_layout = 1;
        }
        perf_fail_closed(ctx, "group-layout");
        if (out) {
            out->severity_milli = 1;
        }
        return 1;
    }
    if (ctx->fd_llc_misses >= 0 && tsd_perf_read_exact(ctx->fd_llc_misses, &llc_now, sizeof(llc_now)) != 0) {
        tsd_log_warn(LOG_COMPONENT, "LLC counter read failed; disabling memory-bound guard and scheduling independent reprobe");
        close(ctx->fd_llc_misses);
        ctx->fd_llc_misses = -1;
        schedule_llc_retry(ctx);
        llc_now = 0;
    }
    if (!ctx->last_group_valid) {
        ctx->last_group_read = rd_now;
        ctx->last_group_valid = 1;
        ctx->last_llc_value = llc_now;
        publish_perf_state(ctx, 1);
        return 0;
    }

    if (!group_order_valid(&ctx->last_group_read, &rd_now)) {
        perf_fail_closed(ctx, "group-counter-regression");
        if (out) {
            out->severity_milli = 1;
        }
        return 1;
    }

    uint64_t enabled_delta = rd_now.time_enabled - ctx->last_group_read.time_enabled;
    uint64_t running_delta = rd_now.time_running - ctx->last_group_read.time_running;
    if (running_delta == 0 || enabled_delta == 0) {
        return handle_no_running_progress(ctx, out);
    }
    ctx->zero_running_samples = 0;

    uint64_t raw_cycles_delta = rd_now.values[0] - ctx->last_group_read.values[0];
    uint64_t raw_insns_delta = rd_now.values[1] - ctx->last_group_read.values[1];
    if (raw_insns_delta == 0) {
        /* Valid enabled group, but the target workload thread was idle. */
        ctx->last_group_read = rd_now;
        ctx->last_llc_value = llc_now;
        publish_perf_state(ctx, 1);
        return 0;
    }

    uint64_t delta_cycles = scale_counter(raw_cycles_delta, enabled_delta, running_delta);
    uint64_t delta_insns = scale_counter(raw_insns_delta, enabled_delta, running_delta);
    if (delta_cycles == 0 || delta_insns == 0) {
        return handle_no_running_progress(ctx, out);
    }
    uint64_t current_cpi = (delta_cycles * 1000) / delta_insns;
    uint64_t now_work = atomic_load_explicit(&g_tsd_workload_iterations, memory_order_relaxed);
    uint64_t delta_work = now_work >= ctx->hw_last_iterations ? now_work - ctx->hw_last_iterations : 0;
    ctx->hw_last_iterations = now_work;
    uint64_t current_work_cost_milli = delta_work
        ? (uint64_t)(((__uint128_t)delta_cycles * 1000u) / delta_work)
        : 0;

    if (!ctx->workload && delta_work == 0) {
        /* In registered-dispatch mode, owner-thread instructions with no
         * completed registered work are not workload performance evidence.
         * Advance the perf cursors so idle/unrelated cycles cannot leak into
         * the next real work sample, while still exporting raw safety telemetry
         * for the admission gate and emergency-temperature path. */
        ctx->last_group_read = rd_now;
        ctx->last_llc_value = llc_now;
        publish_perf_state(ctx, 1);

        tsd_telemetry_sample_t telemetry = {0};
        fetch_fused_telemetry(ctx, &telemetry);
        if (out) {
            out->performance_available = 0;
            out->work_normalized = 0;
            out->work_cost_milli = 0;
            out->temp_available = telemetry.temp_available;
            out->freq_ratio_available = telemetry.freq_ratio_available;
            out->package_temp_millic = telemetry.package_temp_millic;
            out->freq_ratio_milli = telemetry.freq_ratio_milli;
            out->filtered_temp_available = telemetry.filtered_temp_available;
            out->filtered_freq_ratio_available = telemetry.filtered_freq_ratio_available;
            out->filtered_package_temp_millic = telemetry.filtered_package_temp_millic;
            out->filtered_freq_ratio_milli = telemetry.filtered_freq_ratio_milli;
        }
        return 0;
    }

    uint64_t delta_llc = (ctx->fd_llc_misses >= 0 && llc_now >= ctx->last_llc_value)
                             ? (llc_now - ctx->last_llc_value)
                             : 0;
    uint64_t mpki_milli = (delta_llc * MPKI_SCALE) / delta_insns;

    ctx->last_group_read = rd_now;
    ctx->last_llc_value = llc_now;
    publish_perf_state(ctx, 1);

    tsd_telemetry_sample_t telemetry = {0};
    fetch_fused_telemetry(ctx, &telemetry);
    return process_measurement(ctx, out, current_cpi, current_work_cost_milli, mpki_milli, cfg, &telemetry);
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

int tsd_perf_upgrades_allowed(const perf_ctx_t *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->mode == TSD_PERF_MODE_HARDWARE && ctx->hardware_validated;
}

int tsd_perf_check_software_timeout(perf_ctx_t *ctx, int timeout_sec) {
    if (!ctx || timeout_sec <= 0 || ctx->timeout_notified) {
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
    ctx->owner_tid = current_tid();
    ctx->force_software = 1;
    reset_perf_retry(ctx);
    reset_llc_retry(ctx);
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
        ctx->hardware_validated = mode == TSD_PERF_MODE_HARDWARE ? 1 : 0;
        ctx->timeout_notified = 0;
        clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
    }
}

void tsd_perf_test_seed_cpi_reference(perf_ctx_t *ctx, uint64_t baseline_cpi) {
    if (!ctx || baseline_cpi == 0) return;
    ctx->baseline_cpi = baseline_cpi;
    ctx->calibrated_cpi_reference = baseline_cpi;
    ctx->slow_cpi = baseline_cpi;
    ctx->fast_cpi = baseline_cpi;
    memset(ctx->ratio_history, 0, sizeof(ctx->ratio_history));
    ctx->ratio_history_count = 0;
    ctx->ratio_history_cursor = 0;
    ctx->ratio_trimmed_milli = 0;
}

int tsd_perf_test_process_cpi(perf_ctx_t *ctx, uint64_t current_cpi,
                              tsd_thermal_eval_t *out, const tsd_runtime_config *cfg) {
    tsd_telemetry_sample_t telemetry = {0};
    return process_measurement(ctx, out, current_cpi, 0, 0, cfg, &telemetry);
}

void tsd_test_perf_rewind_mode(perf_ctx_t *ctx, int seconds) {
    if (!ctx) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
    if (seconds > 0) {
        ctx->mode_entered_at.tv_sec = ctx->mode_entered_at.tv_sec > seconds
                                          ? ctx->mode_entered_at.tv_sec - seconds
                                          : 0;
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

int tsd_perf_test_group_progress_valid(uint64_t before_enabled,
                                       uint64_t before_running,
                                       uint64_t before_cycles,
                                       uint64_t before_insns,
                                       uint64_t after_enabled,
                                       uint64_t after_running,
                                       uint64_t after_cycles,
                                       uint64_t after_insns) {
    perf_group_read_t before = {.nr = 2, .time_enabled = before_enabled, .time_running = before_running,
                                .values = {before_cycles, before_insns}};
    perf_group_read_t after = {.nr = 2, .time_enabled = after_enabled, .time_running = after_running,
                               .values = {after_cycles, after_insns}};
    return group_baseline_progress_valid(&before, &after);
}

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
