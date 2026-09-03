#!/usr/bin/env python3
from pathlib import Path


def replace_exact(text: str, old: str, new: str, expected: int = 1) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"expected {expected} occurrence(s), found {count}: {old[:100]!r}")
    return text.replace(old, new)


# Keep software-mode work-cost estimates in a clean domain rather than carrying
# hardware CPI EWMAs across a perf-mode transition.
perf_path = Path("src/thermal_perf.c")
perf = perf_path.read_text()
perf = replace_exact(
    perf,
    '''static int allow_software_upgrades(void) {
    const char *env = getenv("TSD_ALLOW_SOFTWARE_UPGRADES");
    return (env && env[0] != '\\0' && strcmp(env, "0") != 0) ? 1 : 0;
}
''',
    '''static int allow_software_upgrades(void) {
    const char *env = getenv("TSD_ALLOW_SOFTWARE_UPGRADES");
    return (env && env[0] != '\\0' && strcmp(env, "0") != 0) ? 1 : 0;
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
''')
perf = replace_exact(
    perf,
    '''    ctx->mode = mode;
    clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
''',
    '''    /* Hardware CPI and software ns/work-item are different physical
     * quantities. Never carry EWMAs/history from one domain into the other. */
    reset_measurement_domain(ctx);
    ctx->mode = mode;
    clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
''')
perf = replace_exact(
    perf,
    '''        uint64_t current_cpi = (delta_ns * 1000ULL) / delta_iters;
        if (current_cpi == 0) {
            current_cpi = ctx->baseline_cpi ? ctx->baseline_cpi : 1000;
        }
        tsd_telemetry_sample_t telemetry = {0};
        fetch_fused_telemetry(ctx, &telemetry);
        return process_measurement(ctx, out, current_cpi, 0, cfg, &telemetry);
''',
    '''        /* Software mode estimates elapsed nanoseconds per completed work
         * item, scaled by 1000. It is a control cost, not hardware CPI. The
         * domain reset on mode entry ensures ratios are only compared against
         * software-mode history. */
        uint64_t software_cost_milli = (delta_ns * 1000ULL) / delta_iters;
        if (software_cost_milli == 0) {
            software_cost_milli = 1;
        }
        tsd_telemetry_sample_t telemetry = {0};
        fetch_fused_telemetry(ctx, &telemetry);
        return process_measurement(ctx, out, software_cost_milli, 0, cfg, &telemetry);
''')
perf_path.write_text(perf)


# Enforce dwell/cooldown using actual monotonic elapsed time. Sample counts stay
# sample-based (down_count/up_count); only time semantics move away from ticks.
simd_path = Path("src/thermal_simd.c")
simd = simd_path.read_text()
simd = replace_exact(
    simd,
    '''static uint64_t compute_dwell_ms_from_ticks(int dwell_ticks) {
    if (dwell_ticks <= 0) {
        return 0;
    }
    uint64_t interval_us = (uint64_t)g_tsd_config.check_interval_us;
    return (interval_us * (uint64_t)dwell_ticks) / 1000ULL;
}

static void record_dwell_metric(simd_width_t width, int dwell_ticks) {
    uint64_t dwell_ms = compute_dwell_ms_from_ticks(dwell_ticks);
    if (dwell_ms > 0) {
        tsd_metrics_exporter_observe_dwell(width, dwell_ms);
    }
}
''',
    '''static uint64_t monotonic_now_ms(void) {
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static uint64_t nonnegative_ms(int value) {
    return value > 0 ? (uint64_t)value : 0;
}

static void record_dwell_metric(simd_width_t width, uint64_t dwell_ms) {
    if (dwell_ms > 0) {
        tsd_metrics_exporter_observe_dwell(width, dwell_ms);
    }
}
''')
simd = replace_exact(
    simd,
    '''    int cooldown = 0;
    int dwell_ticks = 0;
''',
    '''    uint64_t width_since_ms = monotonic_now_ms();
    uint64_t cooldown_until_ms = 0;
''')
simd = replace_exact(
    simd,
    '''        dwell_ticks++;

        /* Fail-closed selections can originate inside telemetry evaluation. */
''',
    '''        uint64_t now_ms = monotonic_now_ms();
        uint64_t dwell_ms = now_ms >= width_since_ms ? now_ms - width_since_ms : 0;

        /* Fail-closed selections can originate inside telemetry evaluation. */
''')
simd = replace_exact(
    simd,
    '''        if (cooldown > 0) {
            cooldown--;
            continue;
        }
        if (dwell_ticks < g_tsd_config.min_dwell_ticks) {
            continue;
        }
''',
    '''        if (now_ms < cooldown_until_ms) {
            continue;
        }
        if (dwell_ms < nonnegative_ms(g_tsd_config.min_dwell_ms)) {
            continue;
        }
''')
simd = replace_exact(
    simd,
    '''        if (cooldown <= 0 && dwell_ticks >= g_tsd_config.min_dwell_ticks && policy_state) {
''',
    '''        if (policy_state) {
''')

for old, new, expected in [
    ("record_dwell_metric(current_width, dwell_ticks);", "record_dwell_metric(current_width, dwell_ms);", 1),
    ("record_dwell_metric(previous, dwell_ticks);", "record_dwell_metric(previous, dwell_ms);", 1),
    ("record_dwell_metric(width, dwell_ticks);", "record_dwell_metric(width, dwell_ms);", 2),
    ("cooldown = g_tsd_config.cooldown_down_ticks;", "cooldown_until_ms = now_ms + nonnegative_ms(g_tsd_config.cooldown_down_ms);", 2),
    ("cooldown = g_tsd_config.cooldown_up_ticks;", "cooldown_until_ms = now_ms + nonnegative_ms(g_tsd_config.cooldown_up_ms);", 1),
    ("dwell_ticks = 0;", "width_since_ms = now_ms;", 4),
]:
    simd = replace_exact(simd, old, new, expected)

simd = replace_exact(
    simd,
    '''                        cooldown = (target < previous) ? g_tsd_config.cooldown_down_ticks
                                                       : g_tsd_config.cooldown_up_ticks;
''',
    '''                        cooldown_until_ms = now_ms + nonnegative_ms(
                            target < previous ? g_tsd_config.cooldown_down_ms
                                              : g_tsd_config.cooldown_up_ms);
''')

if "dwell_ticks" in simd:
    raise SystemExit("unexpected legacy dwell_ticks reference remains")
if "int cooldown =" in simd:
    raise SystemExit("unexpected legacy cooldown counter remains")

simd_path.write_text(simd)
