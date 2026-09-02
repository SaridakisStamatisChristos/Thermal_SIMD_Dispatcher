#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <thermal/simd/thermal_config.h>

static void write_temp_file(const char *contents, char *path, size_t path_len) {
    if (!contents || !path || path_len < 1) {
        fprintf(stderr, "invalid arguments to write_temp_file\n");
        exit(1);
    }
    strncpy(path, "/tmp/tsd_cfgXXXXXX", path_len);
    path[path_len - 1] = '\0';
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        perror("fdopen");
        close(fd);
        exit(1);
    }
    if (fputs(contents, fp) == EOF) {
        perror("fputs");
        fclose(fp);
        exit(1);
    }
    if (fclose(fp) != 0) {
        perror("fclose");
        exit(1);
    }
}

static void test_cli_predictive_and_metrics(void) {
    tsd_runtime_config cfg;
    char *argv[] = {
        "thermal_simd",
        "--temp-ceiling=95",
        "--safety-margin=6",
        "--emergency-margin=15",
        "--predictive-alpha=0.35",
        "--coeff-path=/etc/tsd/coeff.json",
        "--telemetry-interval=60",
        "--telemetry-max-skew=180",
        "--telemetry-ewma=0.4",
        "--telemetry-profile=/etc/tsd/telemetry.json",
        "--metrics-port=9200",
        "--metrics-bind=0.0.0.0",
        "--metrics-cert=/etc/tsd/cert.pem",
        "--metrics-key=/etc/tsd/key.pem",
        "--metrics-ca=/etc/tsd/ca.pem",
        "--metrics-require-client-auth",
        "--metrics-basic-auth=user:pass",
        "--statsd-host=127.0.0.1",
        "--statsd-port=9000",
    };
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    assert(tsd_runtime_config_parse_cli(&cfg, argc, argv) == 0);
    assert(cfg.predictive_temp_ceiling_c == 95);
    assert(cfg.predictive_safety_margin_c == 6);
    assert(cfg.predictive_emergency_margin_c == 15);
    assert(fabs(cfg.predictive_alpha - 0.35) < 0.0001);
    assert(strcmp(cfg.predictive_coeff_path, "/etc/tsd/coeff.json") == 0);
    assert(cfg.telemetry_interval_ms == 60);
    assert(cfg.telemetry_max_skew_ms == 180);
    assert(fabs(cfg.telemetry_ewma_alpha - 0.4) < 0.0001);
    assert(strcmp(cfg.telemetry_profile_path, "/etc/tsd/telemetry.json") == 0);
    assert(cfg.metrics_port == 9200);
    assert(strcmp(cfg.metrics_bind_host, "0.0.0.0") == 0);
    assert(strcmp(cfg.metrics_tls_cert_path, "/etc/tsd/cert.pem") == 0);
    assert(strcmp(cfg.metrics_tls_key_path, "/etc/tsd/key.pem") == 0);
    assert(strcmp(cfg.metrics_tls_ca_path, "/etc/tsd/ca.pem") == 0);
    assert(cfg.metrics_tls_require_client_auth == 1);
    assert(strcmp(cfg.metrics_basic_auth_user, "user") == 0);
    assert(strcmp(cfg.metrics_basic_auth_pass, "pass") == 0);
    assert(strcmp(cfg.statsd_host, "127.0.0.1") == 0);
    assert(cfg.statsd_port == 9000);
}

static void test_service_cli(void) {
    tsd_runtime_config cfg;
    char *argv[] = {
        "thermal_simd",
        "--run-forever",
        "--work-iters=1234",
        "--duration-sec=17",
    };
    assert(tsd_runtime_config_parse_cli(&cfg, 4, argv) == 0);
    assert(cfg.run_forever == 1);
    assert(cfg.work_iters == 1234);
    /* Finite duration remains configured but is ignored by persistent mode. */
    assert(cfg.demo_duration_sec == 17);
}

static void test_config_file_loading(void) {
    char path[64];
    const char *json =
        "{\n"
        "  \"predictive\": {\n"
        "    \"coeff_path\": \"/opt/coeff.json\",\n"
        "    \"temp_ceiling_c\": 90,\n"
        "    \"safety_margin_c\": 5,\n"
        "    \"emergency_margin_c\": 12,\n"
        "    \"alpha\": 0.22\n"
        "  },\n"
        "  \"telemetry\": {\n"
        "    \"profile\": \"/opt/telemetry.json\",\n"
        "    \"interval_ms\": 80,\n"
        "    \"max_skew_ms\": 240,\n"
        "    \"ewma\": 0.3\n"
        "  },\n"
        "  \"metrics\": {\n"
        "    \"bind_address\": \"::\",\n"
        "    \"port\": 9100,\n"
        "    \"tls\": {\n"
        "      \"certificate\": \"/opt/cert.pem\",\n"
        "      \"private_key\": \"/opt/key.pem\"\n"
        "    },\n"
        "    \"basic_auth\": {\n"
        "      \"username\": \"cfguser\",\n"
        "      \"password\": \"cfgpass\"\n"
        "    },\n"
        "    \"statsd\": {\n"
        "      \"host\": \"statsd.local\",\n"
        "      \"port\": 8126\n"
        "    }\n"
        "  }\n"
        "}\n";

    write_temp_file(json, path, sizeof(path));

    tsd_runtime_config cfg;
    char config_arg[80];
    snprintf(config_arg, sizeof(config_arg), "--config=%s", path);
    char *argv[] = {"thermal_simd", config_arg};
    assert(tsd_runtime_config_parse_cli(&cfg, 2, argv) == 0);

    assert(strcmp(cfg.predictive_coeff_path, "/opt/coeff.json") == 0);
    assert(cfg.predictive_temp_ceiling_c == 90);
    assert(cfg.predictive_safety_margin_c == 5);
    assert(cfg.predictive_emergency_margin_c == 12);
    assert(fabs(cfg.predictive_alpha - 0.22) < 0.0001);
    assert(strcmp(cfg.telemetry_profile_path, "/opt/telemetry.json") == 0);
    assert(cfg.telemetry_interval_ms == 80);
    assert(cfg.telemetry_max_skew_ms == 240);
    assert(fabs(cfg.telemetry_ewma_alpha - 0.3) < 0.0001);
    assert(strcmp(cfg.metrics_bind_host, "::") == 0);
    assert(cfg.metrics_port == 9100);
    assert(strcmp(cfg.metrics_tls_cert_path, "/opt/cert.pem") == 0);
    assert(strcmp(cfg.metrics_tls_key_path, "/opt/key.pem") == 0);
    assert(strcmp(cfg.metrics_basic_auth_user, "cfguser") == 0);
    assert(strcmp(cfg.metrics_basic_auth_pass, "cfgpass") == 0);
    assert(strcmp(cfg.statsd_host, "statsd.local") == 0);
    assert(cfg.statsd_port == 8126);

    unlink(path);
}

static void test_cli_overrides_config(void) {
    char path[64];
    const char *json =
        "{\n"
        "  \"predictive\": { \"temp_ceiling_c\": 85 },\n"
        "  \"metrics\": { \"port\": 9300 }\n"
        "}\n";
    write_temp_file(json, path, sizeof(path));

    tsd_runtime_config cfg;
    char config_arg[80];
    snprintf(config_arg, sizeof(config_arg), "--config=%s", path);
    char *argv[] = {
        "thermal_simd",
        config_arg,
        "--temp-ceiling=100",
        "--metrics-port=9400"
    };
    assert(tsd_runtime_config_parse_cli(&cfg, 4, argv) == 0);
    assert(cfg.predictive_temp_ceiling_c == 100);
    assert(cfg.metrics_port == 9400);
    unlink(path);
}

int main(void) {
    test_cli_predictive_and_metrics();
    test_service_cli();
    test_config_file_loading();
    test_cli_overrides_config();
    return 0;
}
