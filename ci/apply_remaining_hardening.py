#!/usr/bin/env python3
from pathlib import Path
import re


def read(path):
    return Path(path).read_text()


def write(path, text):
    Path(path).write_text(text)


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)

# Build systems: compile the C11-owned atomic trampoline state in every path.
p = "CMakeLists.txt"
s = read(p)
s = replace_once(s,
    "    src/patcher/trampoline.cpp\n    src/patcher/trampoline_guard.c\n",
    "    src/patcher/trampoline.cpp\n    src/patcher/trampoline_state.c\n    src/patcher/trampoline_guard.c\n",
    "cmake trampoline state")
write(p, s)

p = "Makefile"
s = read(p)
s = replace_once(s,
    "\tsrc/thermal_cpu.c \\\n\tsrc/patcher/trampoline_guard.c \\\n",
    "\tsrc/thermal_cpu.c \\\n\tsrc/patcher/trampoline_state.c \\\n\tsrc/patcher/trampoline_guard.c \\\n",
    "make trampoline state")
write(p, s)

# C++ patcher: all C11 atomic storage is accessed through the C ABI.
p = "src/patcher/trampoline.cpp"
s = read(p)
raw_defs = """std::atomic<simd_width_t> g_tsd_current_width{SIMD_SSE41};
std::atomic<unsigned char> g_tsd_current_width_byte{static_cast<unsigned char>(SIMD_SSE41)};
std::atomic<int> g_tsd_trampoline_initialized{0};
std::atomic<tsd_patch_slot_t*> g_tsd_active_trampoline{nullptr};
std::atomic<unsigned char> g_tsd_last_patch_attempt{static_cast<unsigned char>(SIMD_SSE41)};
std::atomic<unsigned char> g_tsd_last_patched_width{static_cast<unsigned char>(SIMD_SSE41)};
std::atomic<bool> g_tsd_page_a_effective_writable{false};
std::atomic<bool> g_tsd_page_b_effective_writable{false};
"""
s = replace_once(s, raw_defs, "", "remove C++ raw atomic definitions")
for old, new, label in [
    ("std::atomic_store_explicit(&g_tsd_page_a_effective_writable, true, std::memory_order_release);", "tsd_trampoline_state_set_page_a_writable(1);", "page a true"),
    ("std::atomic_store_explicit(&g_tsd_page_a_effective_writable, false, std::memory_order_release);", "tsd_trampoline_state_set_page_a_writable(0);", "page a false occurrences"),
    ("std::atomic_store_explicit(&g_tsd_page_b_effective_writable, false, std::memory_order_release);", "tsd_trampoline_state_set_page_b_writable(0);", "page b false occurrences"),
]:
    if label.endswith("occurrences"):
        if old not in s:
            raise SystemExit(f"{label}: no matches")
        s = s.replace(old, new)
    else:
        s = replace_once(s, old, new, label)
init_block = """    std::atomic_store_explicit(&g_tsd_active_trampoline, g_slots[SIMD_SSE41], std::memory_order_seq_cst);
    std::atomic_store_explicit(&g_tsd_current_width, SIMD_SSE41, std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_current_width_byte, static_cast<unsigned char>(SIMD_SSE41), std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_trampoline_initialized, 0, std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_last_patch_attempt, static_cast<unsigned char>(SIMD_SSE41), std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_last_patched_width, static_cast<unsigned char>(SIMD_SSE41), std::memory_order_release);
"""
s = replace_once(s, init_block, "    tsd_trampoline_state_reset(g_slots[SIMD_SSE41]);\n", "patcher init state")
s = s.replace("std::atomic_load_explicit(&g_tsd_current_width, std::memory_order_acquire)", "tsd_trampoline_state_current_width()")
s = s.replace("std::atomic_load_explicit(&g_tsd_active_trampoline, std::memory_order_acquire)", "tsd_trampoline_state_active()")
s = s.replace("std::atomic_load_explicit(&g_tsd_trampoline_initialized, std::memory_order_acquire)", "tsd_trampoline_state_initialized()")
s = s.replace("std::atomic_load_explicit(&g_tsd_page_a_effective_writable, std::memory_order_acquire)", "tsd_trampoline_state_page_a_writable()")
s = s.replace("std::atomic_load_explicit(&g_tsd_page_b_effective_writable, std::memory_order_acquire)", "tsd_trampoline_state_page_b_writable()")
s = s.replace("std::atomic_store_explicit(&g_tsd_last_patch_attempt, static_cast<unsigned char>(new_width), std::memory_order_release);", "tsd_trampoline_state_set_last_patch_attempt(new_width);")
publish_block = """        std::atomic_store_explicit(&g_tsd_current_width, new_width, std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_current_width_byte, static_cast<unsigned char>(new_width), std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_active_trampoline, target, std::memory_order_seq_cst);
        std::atomic_store_explicit(&g_tsd_trampoline_initialized, 1, std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_last_patched_width, static_cast<unsigned char>(new_width), std::memory_order_release);
"""
s = replace_once(s, publish_block, "        tsd_trampoline_state_publish_selection(new_width, target);\n", "selection publish")
raw_names = ["g_tsd_current_width", "g_tsd_current_width_byte", "g_tsd_trampoline_initialized",
             "g_tsd_active_trampoline", "g_tsd_last_patch_attempt", "g_tsd_last_patched_width",
             "g_tsd_page_a_effective_writable", "g_tsd_page_b_effective_writable"]
for name in raw_names:
    if name in s:
        raise SystemExit(f"raw C11 atomic leaked into trampoline.cpp: {name}")
write(p, s)

# Other C++ consumers use the representation-safe accessors as well.
for p in ["src/telemetry/fusion_bridge.cpp", "tests/patcher/test_trampoline_security.cpp"]:
    s = read(p)
    replacements = {
        "std::atomic_load_explicit(&g_tsd_current_width, std::memory_order_acquire)": "tsd_trampoline_state_current_width()",
        "std::atomic_load_explicit(&g_tsd_active_trampoline, std::memory_order_acquire)": "tsd_trampoline_state_active()",
        "std::atomic_load_explicit(&g_tsd_trampoline_initialized, std::memory_order_acquire)": "tsd_trampoline_state_initialized()",
        "std::atomic_load_explicit(&g_tsd_last_patch_attempt, std::memory_order_acquire)": "tsd_trampoline_state_last_patch_attempt()",
        "std::atomic_load_explicit(&g_tsd_last_patched_width, std::memory_order_acquire)": "tsd_trampoline_state_last_patched_width()",
    }
    for old, new in replacements.items():
        s = s.replace(old, new)
    for name in raw_names:
        if name in s:
            raise SystemExit(f"raw C11 atomic leaked into {p}: {name}")
    write(p, s)

# Perf engine: monotonic retry deadlines, absolute live calibration, strict
# software fail-closed semantics, and effective degraded thresholds.
p = "src/thermal_perf.c"
s = read(p)
s = replace_once(s, "    uint64_t baseline_cpi;\n", "    uint64_t baseline_cpi;\n    uint64_t calibrated_cpi_reference;\n", "calibrated reference field")
s = s.replace("    time_t perf_retry_deadline;", "    uint64_t perf_retry_deadline_ns;")
s = s.replace("    time_t llc_retry_deadline;", "    uint64_t llc_retry_deadline_ns;")
allow_fn = """static int allow_software_upgrades(void) {
    const char *env = getenv("TSD_ALLOW_SOFTWARE_UPGRADES");
    return (env && env[0] != '\\0' && strcmp(env, "0") != 0) ? 1 : 0;
}

"""
s = replace_once(s, allow_fn, "", "remove software upgrade escape hatch")
s = replace_once(s,
    "    ctx->slow_cpi = 0;\n    ctx->fast_cpi = 0;\n",
    "    ctx->slow_cpi = 0;\n    ctx->fast_cpi = 0;\n    ctx->calibrated_cpi_reference = 0;\n",
    "reset calibrated reference")
retry_start = s.index("static time_t now_seconds(void)")
retry_end = s.index("static void perf_set_mode", retry_start)
retry_new = r'''static uint64_t monotonic_now_ns(void) {
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

'''
s = s[:retry_start] + retry_new + s[retry_end:]
s = replace_once(s,
    """    if (previous == mode) {
        clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
        publish_perf_state(ctx, mode == TSD_PERF_MODE_HARDWARE && ctx->hardware_validated);
        return;
    }
""",
    """    if (previous == mode) {
        /* Re-publishing the same state must not restart degraded-mode timeout. */
        publish_perf_state(ctx, mode == TSD_PERF_MODE_HARDWARE && ctx->hardware_validated);
        return;
    }
""",
    "same-mode timeout semantics")
old_force = """        if (!allow_software_upgrades() &&
            atomic_load_explicit(&g_tsd_current_width, memory_order_relaxed) != SIMD_SSE41) {
            if (tsd_trampoline_patch(SIMD_SSE41) == 0) {
                tsd_log_warn(LOG_COMPONENT, "event=perf_mode action=forced-width width=SSE4.1 reason=%s", why);
            }
        }
"""
new_force = """        if (atomic_load_explicit(&g_tsd_current_width, memory_order_acquire) != SIMD_SSE41) {
            if (tsd_trampoline_patch(SIMD_SSE41) == 0) {
                tsd_log_warn(LOG_COMPONENT, "event=perf_mode action=forced-width width=SSE4.1 reason=%s", why);
            } else {
                tsd_log_error(LOG_COMPONENT, "event=perf_mode action=force-width-failed width=SSE4.1 reason=%s errno=%d",
                              why, errno);
            }
        }
"""
s = replace_once(s, old_force, new_force, "force SSE in software mode")
s = replace_once(s,
    """    if (!ctx) {
        return 0;
    }
    ctx->fast_cpi = tsd_update_ewma(ctx->fast_cpi, current_cpi, FAST_EWMA_SHIFT);
""",
    """    if (!ctx) {
        return 0;
    }
    if (ctx->calibrated_cpi_reference == 0 && current_cpi > 0) {
        ctx->calibrated_cpi_reference = current_cpi;
    }
    ctx->fast_cpi = tsd_update_ewma(ctx->fast_cpi, current_cpi, FAST_EWMA_SHIFT);
""",
    "live CPI calibration")
old_ratio = """    uint64_t reference_cpi = ctx->slow_cpi ? ctx->slow_cpi : (ctx->baseline_cpi ? ctx->baseline_cpi : 1);
    __uint128_t ratio_num = (__uint128_t)current_cpi * 1000u;
    uint64_t ratio_milli = (uint64_t)((ratio_num + reference_cpi / 2) / reference_cpi);
"""
new_ratio = """    uint64_t reference_cpi = ctx->slow_cpi ? ctx->slow_cpi : (ctx->baseline_cpi ? ctx->baseline_cpi : 1);
    __uint128_t ratio_num = (__uint128_t)current_cpi * 1000u;
    uint64_t adaptive_ratio_milli = (uint64_t)((ratio_num + reference_cpi / 2) / reference_cpi);
    uint64_t absolute_reference = ctx->calibrated_cpi_reference ? ctx->calibrated_cpi_reference : reference_cpi;
    uint64_t absolute_ratio_milli = (uint64_t)((ratio_num + absolute_reference / 2) / absolute_reference);
    /* A slow EWMA must not normalize sustained degradation back to 1.0. Keep a
     * frozen live calibration and use the more conservative of the two ratios. */
    uint64_t ratio_milli = adaptive_ratio_milli > absolute_ratio_milli
                               ? adaptive_ratio_milli : absolute_ratio_milli;
"""
s = replace_once(s, old_ratio, new_ratio, "absolute degradation ratio")
s = replace_once(s,
    "    uint64_t dynamic_threshold = cfg ? cfg->down_ratio_milli : 1500;\n",
    "    uint64_t dynamic_threshold = cfg ? tsd_runtime_config_effective_down_ratio_milli(cfg) : 1500;\n",
    "effective degraded threshold")
old_upgrades = """    return (ctx->mode == TSD_PERF_MODE_HARDWARE && ctx->hardware_validated) ||
           (ctx->mode == TSD_PERF_MODE_SOFTWARE && allow_software_upgrades());
"""
s = replace_once(s, old_upgrades,
    "    return ctx->mode == TSD_PERF_MODE_HARDWARE && ctx->hardware_validated;\n",
    "hardware-only upgrade authorization")
if "allow_software_upgrades" in s or "now_seconds()" in s or "perf_retry_deadline;" in s or "llc_retry_deadline;" in s:
    raise SystemExit("legacy perf safety path remains")
write(p, s)

# ARX reload regression: make the reloaded model exceed the conservative latest
# temperature floor, rather than asserting an under-predicting model can do so.
p = "tests/policy/test_arx_model.cpp"
s = read(p)
s = replace_once(s,
    '  "bias": 5000.0,\n  "ar_temperature": [0.9],',
    '  "bias": 20000.0,\n  "ar_temperature": [1.0],',
    "ARX reload fixture")
write(p, s)

# Metrics exporter: escape label values, rollback partial thread startup, and
# contain all C++ exceptions at the extern-C boundary.
p = "src/observability/metrics.cpp"
s = read(p)
s = replace_once(s, "#include <condition_variable>\n", "#include <condition_variable>\n#include <cerrno>\n#include <cstdlib>\n#include <stdexcept>\n", "metrics exception includes")
marker = """std::string sanitize_for_statsd(const std::string &value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_') {
            result.push_back(ch);
        } else {
            result.push_back('_');
        }
    }
    return result;
}
"""
escape = marker + """
std::string escape_prometheus_label(const std::string &value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\\\': result += "\\\\\\\\"; break;
            case '\"': result += "\\\\\\\""; break;
            case '\\n': result += "\\\\n"; break;
            case '\\r': result.push_back('_'); break;
            default: result.push_back(ch); break;
        }
    }
    return result;
}
"""
s = replace_once(s, marker, escape, "Prometheus label escaping")
s = s.replace('<< entry.first.sensor\n                 << "\\\",socket=', '<< escape_prometheus_label(entry.first.sensor)\n                 << "\\\",socket=')
if s.count("escape_prometheus_label(entry.first.sensor)") < 4:
    raise SystemExit("not all sensor labels were escaped")
start_old = """        observability::StatsdExporter::instance().configure(local_config.statsd_host, local_config.statsd_port);
        listen_fd_ = fd;
        listen_port_ = ntohs(actual.sin_port);
        bind_address_ = local_config.bind_address;
        config_ = local_config;
        running_.store(true, std::memory_order_release);

        workers_.clear();
        workers_.reserve(kWorkerCount);
        for (size_t i = 0; i < kWorkerCount; ++i) {
            workers_.emplace_back(&PrometheusExporter::worker_loop, this);
        }
        server_thread_ = std::thread(&PrometheusExporter::serve, this, fd);
        return 0;
"""
start_new = """        try {
            observability::StatsdExporter::instance().configure(local_config.statsd_host, local_config.statsd_port);
            listen_fd_ = fd;
            listen_port_ = ntohs(actual.sin_port);
            bind_address_ = local_config.bind_address;
            config_ = local_config;
            running_.store(true, std::memory_order_release);

            workers_.clear();
            workers_.reserve(kWorkerCount);
#ifdef TSD_ENABLE_TESTS
            long fail_after = -1;
            if (const char *value = std::getenv("TSD_TEST_METRICS_FAIL_THREAD_AFTER")) {
                char *end = nullptr;
                errno = 0;
                long parsed = std::strtol(value, &end, 10);
                if (errno == 0 && end != value && *end == '\\0') fail_after = parsed;
            }
#endif
            for (size_t i = 0; i < kWorkerCount; ++i) {
#ifdef TSD_ENABLE_TESTS
                if (fail_after >= 0 && static_cast<long>(i) >= fail_after) {
                    throw std::runtime_error("injected metrics worker startup failure");
                }
#endif
                workers_.emplace_back(&PrometheusExporter::worker_loop, this);
            }
            server_thread_ = std::thread(&PrometheusExporter::serve, this, fd);
            return 0;
        } catch (...) {
            running_.store(false, std::memory_order_release);
            (void)::shutdown(fd, SHUT_RDWR);
            client_cv_.notify_all();
            for (auto &worker : workers_) {
                if (worker.joinable()) worker.join();
            }
            workers_.clear();
            if (listen_fd_ == fd) listen_fd_ = -1;
            listen_port_ = 0;
            ::close(fd);
            observability::StatsdExporter::instance().shutdown();
            destroy_tls();
            return -1;
        }
"""
s = replace_once(s, start_old, start_new, "transactional metrics startup")
s = replace_once(s, "    ~PrometheusExporter() { stop(); }\n", "    ~PrometheusExporter() { try { stop(); } catch (...) {} }\n", "noexcept metrics destructor")
s = replace_once(s,
    """            serve_client(client);
            ::close(client);
""",
    """            try {
                serve_client(client);
            } catch (...) {
                /* A malformed/hostile request or allocation failure must kill
                 * only this client, never the exporter process. */
            }
            ::close(client);
""",
    "metrics worker exception containment")
extern_start = s.index('extern "C" {')
extern_block = s[extern_start:]
new_extern = r'''extern "C" {

int tsd_metrics_exporter_start_with_config(const tsd_metrics_exporter_config_t *config) {
    try {
        ExporterConfig exporter_config;
        if (config) {
            if (config->bind_address) exporter_config.bind_address = config->bind_address;
            exporter_config.port = config->port;
            if (config->tls) {
                exporter_config.tls.enabled = true;
                if (config->tls->certificate_path) exporter_config.tls.certificate = config->tls->certificate_path;
                if (config->tls->private_key_path) exporter_config.tls.key = config->tls->private_key_path;
                if (config->tls->ca_certificate_path) exporter_config.tls.ca = config->tls->ca_certificate_path;
                exporter_config.tls.require_client_auth = config->tls->require_client_auth != 0;
            }
            if (config->basic_auth) {
                exporter_config.auth.enabled = true;
                if (config->basic_auth->username) exporter_config.auth.username = config->basic_auth->username;
                if (config->basic_auth->password) exporter_config.auth.password = config->basic_auth->password;
            }
            if (config->statsd_host) exporter_config.statsd_host = config->statsd_host;
            exporter_config.statsd_port = config->statsd_port;
        }
        return PrometheusExporter::instance().start(exporter_config);
    } catch (...) {
        errno = EIO;
        return -1;
    }
}

int tsd_metrics_exporter_start(const char *bind_address, uint16_t port) {
    try {
        tsd_metrics_exporter_config_t config{};
        config.bind_address = bind_address;
        config.port = port;
        return tsd_metrics_exporter_start_with_config(&config);
    } catch (...) {
        errno = EIO;
        return -1;
    }
}

void tsd_metrics_exporter_stop(void) {
    try { PrometheusExporter::instance().stop(); } catch (...) {}
}

uint16_t tsd_metrics_exporter_listen_port(void) {
    try { return PrometheusExporter::instance().listen_port(); } catch (...) { return 0; }
}

void tsd_metrics_exporter_record_patch(simd_width_t from, simd_width_t to, int rc, uint64_t dwell_ms) {
    try { PrometheusExporter::instance().registry().record_patch(from, to, rc, dwell_ms); } catch (...) {}
}

void tsd_metrics_exporter_observe_dwell(simd_width_t width, uint64_t dwell_ms) {
    try { PrometheusExporter::instance().registry().observe_dwell(width, dwell_ms); } catch (...) {}
}

void tsd_metrics_exporter_record_sensor_health(const char *sensor_name,
                                               int socket,
                                               double health,
                                               double quality,
                                               int valid) {
    if (!sensor_name) return;
    try {
        PrometheusExporter::instance().registry().record_sensor_health(sensor_name, socket, health, quality, valid != 0);
    } catch (...) {}
}

} // extern "C"
'''
if not extern_block.rstrip().endswith('} // extern "C"'):
    raise SystemExit("unexpected metrics extern C tail")
s = s[:extern_start] + new_extern
write(p, s)

# Metrics regressions: injected partial startup must rollback, and hostile label
# bytes must be escaped in Prometheus text format.
p = "tests/observability/test_metrics_exporter.cpp"
s = read(p)
insert_before = """    if (tsd_metrics_exporter_start_with_config(&config) != 0) fail("metrics exporter failed to start");
"""
insert = """    tsd_metrics_exporter_config_t rollback_config{};
    rollback_config.bind_address = "127.0.0.1";
    rollback_config.port = 0;
    (void)::setenv("TSD_TEST_METRICS_FAIL_THREAD_AFTER", "1", 1);
    if (tsd_metrics_exporter_start_with_config(&rollback_config) == 0) {
        fail("injected partial metrics startup unexpectedly succeeded");
    }
    if (tsd_metrics_exporter_listen_port() != 0) fail("failed metrics startup leaked listener state");
    (void)::unsetenv("TSD_TEST_METRICS_FAIL_THREAD_AFTER");

""" + insert_before
s = replace_once(s, insert_before, insert, "metrics rollback regression")
needle = """    HttpResponse unauth = https_request(port, "/metrics", "", ca_crt);
"""
s = replace_once(s, needle,
    """    tsd_metrics_exporter_record_sensor_health("pkg\\\"line\\nslash\\\\sensor", 0, 1.0, 1.0, 1);

""" + needle,
    "hostile sensor label fixture")
old_check = """        metrics.body.find("channel=\\\"filtered_control\\\"") == std::string::npos) {
        fail("metrics response invalid or missing temperature channels");
    }
"""
new_check = """        metrics.body.find("channel=\\\"filtered_control\\\"") == std::string::npos ||
        metrics.body.find("sensor=\\\"pkg\\\\\\\"line\\\\nslash\\\\\\\\sensor\\\"") == std::string::npos) {
        fail("metrics response invalid, unescaped, or missing temperature channels");
    }
"""
s = replace_once(s, old_check, new_check, "Prometheus label escape regression")
write(p, s)

# Documentation: state the strict safety/model contract and coefficient provenance.
p = "README.md"
s = read(p)
section = r'''

## Runtime safety invariants and model provenance

The adaptive runtime is deliberately fail-closed. Registered kernel execution and
live safety-state mutation are linearized by a process-wide safety gate; loss of
hardware perf authority or fresh raw package-temperature authority revokes wider
SIMD and forces SSE4.1. Cooldown/minimum-dwell policy never suppresses emergency
thermal observation, and software perf mode is diagnostic/degraded operation only:
it cannot authorize AVX2 or AVX-512.

The default `config/controller_coeffs.json` is a conservative reference controller
profile, not a universally calibrated physical plant model. Its ARX terms and
explicit per-width thermal/performance effects are engineering priors used to make
model-assisted discrete width decisions. Production deployments should calibrate
and validate coefficients on the target CPU/package/workload, preserve the raw
thermal safety channel independently of the forecast, and use HIL evidence before
enabling AVX-512 policy. The controller never treats a wider SIMD step as a cooling
action; width temperature cost is non-negative by construction.
'''
if "## Runtime safety invariants and model provenance" not in s:
    s += section
write(p, s)

Path("config/COEFFICIENTS.md").write_text(r'''# Controller coefficient provenance

`controller_coeffs.json` ships as a conservative reference profile for the
model-assisted discrete SIMD-width controller. It is **not** claimed to be a
CPU-independent physical model and should not be interpreted as measured silicon
characterization.

The ARX terms provide a bounded forecast over recent telemetry. The fields
`width_temperature_millic_per_step` and `width_performance_benefit_milli_per_step`
make SIMD width an explicit control input. The default values are engineering
priors chosen to make widening thermally costly rather than allowing the optimizer
to infer that a wider width can cool the package from the sign of an SLO error.

For production use, calibrate these coefficients on each target CPU/package and
representative workload, validate them against held-out traces, and verify the
result with hardware-in-the-loop runs. Raw package temperature remains an
independent fail-closed safety signal and is never replaced by the model forecast.
''')

# Cross-language representation audit: no C++ TU may name a raw C11 atomic.
for path in list(Path("src").rglob("*.cpp")) + list(Path("tests").rglob("*.cpp")):
    text = path.read_text()
    for name in raw_names:
        if name in text:
            raise SystemExit(f"C++ raw trampoline atomic reference remains in {path}: {name}")

print("remaining hardening transform applied")
