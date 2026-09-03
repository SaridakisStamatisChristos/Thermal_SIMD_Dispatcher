#!/usr/bin/env python3
from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))

# TSD_ENABLE_TESTS scripted perf data must be usable with a validated hardware
# test state. Move the deterministic script hook ahead of the production mode
# split; production builds do not compile this branch.
p = Path("src/thermal_perf.c")
s = p.read_text()
start_marker = "#ifdef TSD_ENABLE_TESTS\n        if (g_test_perf_script.enabled && g_test_perf_script.count > 0) {"
start = s.find(start_marker)
if start < 0:
    raise SystemExit("scripted perf block start not found")
end_marker = "#endif\n        if (!ctx->software_adaptation) {"
end = s.find(end_marker, start)
if end < 0:
    raise SystemExit("scripted perf block end not found")
block = s[start:end + len("#endif\n")]
s = s[:start] + s[end + len("#endif\n"):]
insert_marker = """    if (out) {
        memset(out, 0, sizeof(*out));
    }

"""
pos = s.find(insert_marker)
if pos < 0:
    raise SystemExit("perf evaluate insertion point not found")
# Normalize indentation from the former software-only nesting.
block = block.replace("\n        ", "\n    ")
s = s[:pos + len(insert_marker)] + block + s[pos + len(insert_marker):]
p.write_text(s)

# The monitor scenario is now explicitly a validated-hardware policy harness;
# software mode is tested separately as permanently fail-closed.
replace_once(
    "tests/test_thermal_simd.c",
    """    tsd_test_measure_baseline(ctx);

    /*
     * The production runtime now treats package temperature as explicit
""",
    """    tsd_test_measure_baseline(ctx);
    tsd_test_perf_set_mode(ctx, TSD_PERF_MODE_HARDWARE);

    /*
     * The production runtime now treats package temperature as explicit
""",
    "monitor validated hardware harness",
)

# reloadCoefficients intentionally re-resolves the configured/env path. Point
# that public reload selector at the rewritten fixture instead of relying on the
# debug-only path override to survive the re-resolution.
p = Path("tests/policy/test_arx_model.cpp")
s = p.read_text()
if "#include <cstdlib>" not in s:
    s = s.replace("#include <cstdio>\n", "#include <cstdio>\n#include <cstdlib>\n", 1)
old = """    assert(controller.reloadCoefficients());

    populateSample(sample, 1650, 81000);
"""
new = """    assert(::setenv("TSD_PREDICTIVE_COEFF_PATH", base_path.c_str(), 1) == 0);
    assert(controller.reloadCoefficients());
    (void)::unsetenv("TSD_PREDICTIVE_COEFF_PATH");

    populateSample(sample, 1650, 81000);
"""
count = s.count(old)
if count != 1:
    raise SystemExit(f"ARX reload selector: expected one match, found {count}")
s = s.replace(old, new, 1)
p.write_text(s)

# The hostile Prometheus-label sample emits a StatsD health metric as a side
# effect. Verify the transition packet first, then publish the hostile label and
# fetch Prometheus text in a separate request.
p = Path("tests/observability/test_metrics_exporter.cpp")
s = p.read_text()
s = s.replace(
    "    tsd_metrics_exporter_record_sensor_health(\"pkg\\\"line\\nslash\\\\sensor\", 0, 1.0, 1.0, 1);\n\n",
    "",
    1,
)
s = s.replace(
    " ||\n        metrics.body.find(\"sensor=\\\"pkg\\\\\\\"line\\\\nslash\\\\\\\\sensor\\\"\") == std::string::npos",
    "",
    1,
)
needle = """    if (statsd_payload.find("tsd.patch_transition.avx2.avx512.success") == std::string::npos) {
        fail("statsd payload missing transition");
    }

    tsd_metrics_exporter_stop();
"""
replacement = """    if (statsd_payload.find("tsd.patch_transition.avx2.avx512.success") == std::string::npos) {
        fail("statsd payload missing transition");
    }

    tsd_metrics_exporter_record_sensor_health("pkg\\\"line\\nslash\\\\sensor", 0, 1.0, 1.0, 1);
    HttpResponse escaped_metrics = https_request(port, "/metrics", credentials, ca_crt);
    if (escaped_metrics.status != 200 ||
        escaped_metrics.body.find("sensor=\\\"pkg\\\\\\\"line\\\\nslash\\\\\\\\sensor\\\"") == std::string::npos) {
        fail("Prometheus sensor label was not escaped");
    }

    tsd_metrics_exporter_stop();
"""
count = s.count(needle)
if count != 1:
    raise SystemExit(f"metrics escape reorder: expected one match, found {count}")
s = s.replace(needle, replacement, 1)
p.write_text(s)

print("strict-hardening regression harnesses fixed")
