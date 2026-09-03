#!/usr/bin/env python3
from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))

# Freeze the absolute CPI reference at the validated baseline, not at the first
# post-transition sample (which may already be degraded).
p = "src/thermal_perf.c"
replace_once(p,
    "    ctx->fast_cpi = 0;\n    ctx->calibrated_cpi_reference = 0;\n    ctx->slow_llc_mpki = 0;\n",
    "    ctx->fast_cpi = 0;\n    ctx->slow_llc_mpki = 0;\n",
    "do not erase validated CPI calibration on generic domain reset")
replace_once(p,
    "    if (mode == TSD_PERF_MODE_SOFTWARE) {\n        ctx->hardware_validated = 0;\n",
    "    if (mode == TSD_PERF_MODE_SOFTWARE) {\n        /* Hardware CPI is not comparable to software ns/work-item. */\n        ctx->calibrated_cpi_reference = 0;\n        ctx->hardware_validated = 0;\n",
    "clear calibration only on software entry")
replace_once(p,
    "    ctx->baseline_cpi = (delta_cycles * 1000) / delta_insns;\n",
    "    ctx->baseline_cpi = (delta_cycles * 1000) / delta_insns;\n    ctx->calibrated_cpi_reference = ctx->baseline_cpi ? ctx->baseline_cpi : 1;\n",
    "freeze hardware baseline CPI")
replace_once(p,
    "        ctx->baseline_cpi = surrogate_cpi;\n        ctx->baseline_llc_mpki_milli = 1000;\n",
    "        ctx->baseline_cpi = surrogate_cpi;\n        ctx->calibrated_cpi_reference = surrogate_cpi;\n        ctx->baseline_llc_mpki_milli = 1000;\n",
    "freeze software-domain baseline cost")

# Test-only hooks directly exercise long-run ratio behavior without depending on
# perf_event_open availability on CI hosts.
header = Path("include/thermal/simd/thermal_perf.h")
h = header.read_text()
needle = """void tsd_perf_test_set_mode(perf_ctx_t *ctx, tsd_perf_mode_t mode);
void tsd_perf_test_set_read_streams(const tsd_perf_test_read_stream_t *streams, size_t count);
"""
replacement = """void tsd_perf_test_set_mode(perf_ctx_t *ctx, tsd_perf_mode_t mode);
void tsd_perf_test_seed_cpi_reference(perf_ctx_t *ctx, uint64_t baseline_cpi);
int tsd_perf_test_process_cpi(perf_ctx_t *ctx, uint64_t current_cpi,
                              tsd_thermal_eval_t *out, const tsd_runtime_config *cfg);
void tsd_perf_test_set_read_streams(const tsd_perf_test_read_stream_t *streams, size_t count);
"""
if h.count(needle) != 1:
    raise SystemExit("perf test hook header insertion failed")
header.write_text(h.replace(needle, replacement, 1))

src = Path(p)
s = src.read_text()
needle = """void tsd_perf_test_set_mode(perf_ctx_t *ctx, tsd_perf_mode_t mode) {
    if (ctx) {
        ctx->mode = mode;
        ctx->hardware_validated = mode == TSD_PERF_MODE_HARDWARE ? 1 : 0;
        ctx->timeout_notified = 0;
        clock_gettime(CLOCK_MONOTONIC, &ctx->mode_entered_at);
    }
}

"""
replacement = needle + """void tsd_perf_test_seed_cpi_reference(perf_ctx_t *ctx, uint64_t baseline_cpi) {
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
    return process_measurement(ctx, out, current_cpi, 0, cfg, &telemetry);
}

"""
if s.count(needle) != 1:
    raise SystemExit("perf test hook implementation insertion failed")
src.write_text(s.replace(needle, replacement, 1))

# Regression: after enough samples for the slow EWMA to substantially converge,
# an absolute 2x CPI degradation must still be visible against the frozen baseline.
p = Path("tests/perf/test_perf_resilience.c")
s = p.read_text()
insert_before = """static void test_group_progress_requires_actual_runtime(void) {
"""
test = """static void test_sustained_degradation_not_normalized_away(void) {
    tsd_runtime_config_set_defaults(&g_tsd_config);
    perf_ctx_t *ctx = tsd_perf_test_create_dummy_context();
    assert(ctx != NULL);
    tsd_perf_test_seed_cpi_reference(ctx, 1000);

    tsd_thermal_eval_t eval = {0};
    int throttled = 0;
    for (int i = 0; i < 128; ++i) {
        throttled = tsd_perf_test_process_cpi(ctx, 2000, &eval, &g_tsd_config);
    }
    assert(eval.ratio_milli >= 1900);
    assert(eval.trimmed_ratio_milli >= 1900);
    assert(eval.severity_milli > 0);
    assert(throttled != 0);
    tsd_perf_test_destroy_dummy_context(ctx);
}

"""
if s.count(insert_before) != 1:
    raise SystemExit("sustained degradation test insertion failed")
s = s.replace(insert_before, test + insert_before, 1)
s = s.replace("    test_software_mode_never_authorizes_upgrades();\n    test_group_progress_requires_actual_runtime();\n",
              "    test_software_mode_never_authorizes_upgrades();\n    test_sustained_degradation_not_normalized_away();\n    test_group_progress_requires_actual_runtime();\n", 1)
p.write_text(s)

# Remove obsolete software-upgrade override usage from test harnesses. The one
# remaining mention in perf_resilience is a deliberate regression proving that
# the legacy variable is ignored.
for filename in ["tests/test_thermal_simd.c", "tests/stress/stress_common.c", "tests/runtime/test_runtime_lifecycle.c"]:
    p = Path(filename)
    s = p.read_text()
    lines = [line for line in s.splitlines(True) if "TSD_ALLOW_SOFTWARE_UPGRADES" not in line]
    p.write_text("".join(lines))

# Bring operator/controller docs in line with the strict hardware-only authority.
doc_replacements = {
    "docs/runbooks/sensor-failure.md": [
        ("Hardware perf loss enters software/degraded mode and blocks wider upgrades by\ndefault. Missing or stale raw temperature also blocks wider authorization.\nDo not set `TSD_ALLOW_SOFTWARE_UPGRADES` during incident response; it is an\nexplicit override of the conservative behavior. Liveness remains healthy so\n",
         "Hardware perf loss enters software/degraded mode and blocks wider upgrades\nunconditionally. Missing or stale raw temperature also blocks wider authorization.\nThe historical software-upgrade override is ignored by the runtime and cannot\nrestore wider SIMD authority. Liveness remains healthy so\n"),
    ],
    "docs/telemetry-fusion.md": [
        ("3. fails closed to SSE4.1 by default;\n4. continuously denies wider SIMD while software mode remains active unless `TSD_ALLOW_SOFTWARE_UPGRADES` is explicitly enabled;\n5.",
         "3. fails closed to SSE4.1;\n4. continuously denies wider SIMD for the entire time software mode remains active;\n5."),
    ],
    "docs/model-provenance.md": [
        ("primary perf-counter loss enters software/degraded mode and blocks wider SIMD by default;",
         "primary perf-counter loss enters software/degraded mode and blocks wider SIMD unconditionally;"),
    ],
    "docs/testing-matrix.md": [
        ("continuous software-mode upgrade authorization", "hardware-only upgrade authorization and software fail-closed behavior"),
    ],
}
for filename, replacements in doc_replacements.items():
    p = Path(filename)
    s = p.read_text()
    for old, new in replacements:
        if old not in s:
            raise SystemExit(f"documentation phrase not found in {filename}: {old[:60]!r}")
        s = s.replace(old, new, 1)
    p.write_text(s)

# predictive-controller.md wording varies slightly; replace the specific legacy
# environment reference without relying on a whole paragraph match.
p = Path("docs/predictive-controller.md")
s = p.read_text()
if "TSD_ALLOW_SOFTWARE_UPGRADES" in s:
    s = s.replace("unless `TSD_ALLOW_SOFTWARE_UPGRADES` is explicitly enabled", "and software mode never authorizes a wider target")
    s = s.replace("under `TSD_ALLOW_SOFTWARE_UPGRADES`", "in software mode (wider targets remain disabled)")
p.write_text(s)

print("final audit cleanup applied")
