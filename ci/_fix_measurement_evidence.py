#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)

p = ROOT / "src/thermal_perf.c"
s = p.read_text(encoding="utf-8")
s = replace_once(
    s,
"""        tsd_telemetry_sample_t telemetry = {0};
        fetch_fused_telemetry(ctx, &telemetry);
        return process_measurement(ctx, out, software_cost_milli, 0, 0, cfg, &telemetry);
    }
""",
"""        tsd_telemetry_sample_t telemetry = {0};
        fetch_fused_telemetry(ctx, &telemetry);
        int software_rc = process_measurement(ctx, out, software_cost_milli, 0, 0, cfg, &telemetry);
        /* Software mode is a fail-closed observability domain, never an
         * authority for normal width optimization or predictive history. */
        if (out) out->performance_available = 0;
        return software_rc;
    }
""",
    "software evidence gate")

s = replace_once(
    s,
"""    uint64_t current_work_cost_milli = delta_work
        ? (uint64_t)(((__uint128_t)delta_cycles * 1000u) / delta_work)
        : 0;
    uint64_t delta_llc = (ctx->fd_llc_misses >= 0 && llc_now >= ctx->last_llc_value)
""",
"""    uint64_t current_work_cost_milli = delta_work
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
""",
    "registered idle evidence gate")
p.write_text(s, encoding="utf-8")

p = ROOT / "tests/test_thermal_simd.c"
s = p.read_text(encoding="utf-8")
s = replace_once(
    s,
"""    if (triggered != 0 || eval.cpi_milli != 1000 ||
        tsd_test_perf_get_last_llc_value(ctx) != llc_stable) {""",
"""    if (triggered != 0 || eval.performance_available != 0 || eval.cpi_milli != 0 ||
        tsd_test_perf_get_last_llc_value(ctx) != llc_stable) {""",
    "stable no-work expectation")
s = replace_once(
    s,
"""    if (!tsd_test_perf_get_last_group_valid(ctx) ||
        tsd_test_perf_get_last_llc_value(ctx) != llc_hot ||
        eval.cpi_milli != 2000 || triggered == 0) {""",
"""    if (!tsd_test_perf_get_last_group_valid(ctx) ||
        tsd_test_perf_get_last_llc_value(ctx) != llc_hot ||
        eval.performance_available != 0 || eval.cpi_milli != 0 || triggered != 0) {""",
    "hot no-work expectation")
p.write_text(s, encoding="utf-8")

print("measurement evidence hardening applied")
