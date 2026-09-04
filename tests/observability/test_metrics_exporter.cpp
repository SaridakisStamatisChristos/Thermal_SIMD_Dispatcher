#include <observability/metrics_exporter.h>
#include <observability/telemetry_state.h>

#include <thermal/simd/simd_width.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

void fail(const std::string &message) {
    std::cerr << "test failure: " << message << std::endl;
    std::exit(1);
}

struct StatsdCapture {
    int socket{-1};
    uint16_t port{0};
    std::future<std::string> future;
    std::thread thread;
};

StatsdCapture start_statsd_capture() {
    StatsdCapture capture;
    capture.socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (capture.socket < 0) fail("unable to create statsd socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(capture.socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        fail("bind statsd socket failed");
    }
    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (::getsockname(capture.socket, reinterpret_cast<sockaddr*>(&actual), &len) != 0) {
        fail("getsockname statsd socket failed");
    }
    capture.port = ntohs(actual.sin_port);

    struct timeval tv {5, 0};
    (void)::setsockopt(capture.socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::promise<std::string> promise;
    capture.future = promise.get_future();
    int fd = capture.socket;
    capture.thread = std::thread([fd, p = std::move(promise)]() mutable {
        char buffer[512];
        ssize_t n = ::recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            buffer[n] = '\0';
            p.set_value(std::string(buffer, static_cast<size_t>(n)));
        } else {
            p.set_value(std::string());
        }
    });
    return capture;
}

void stop_statsd_capture(StatsdCapture &capture) {
    if (capture.socket >= 0) {
        ::close(capture.socket);
        capture.socket = -1;
    }
    if (capture.thread.joinable()) capture.thread.join();
}

struct HttpResponse {
    int status{0};
    std::string body;
};

HttpResponse https_request(uint16_t port,
                           const std::string &path,
                           const std::string &auth_header,
                           const std::string &ca_path) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) fail("unable to create SSL context");
    if (SSL_CTX_load_verify_locations(ctx, ca_path.c_str(), nullptr) <= 0) fail("unable to load CA");
    SSL *ssl = SSL_new(ctx);
    if (!ssl) fail("unable to allocate SSL");
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) fail("unable to create tcp socket");
    struct timeval tv {5, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) fail("inet_pton failed");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) fail("connect failed");
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, "127.0.0.1");
    if (SSL_connect(ssl) <= 0) fail("SSL_connect failed");
    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: 127.0.0.1\r\n";
    request << "Connection: close\r\n";
    if (!auth_header.empty()) request << "Authorization: " << auth_header << "\r\n";
    request << "\r\n";
    std::string req = request.str();
    if (SSL_write(ssl, req.data(), static_cast<int>(req.size())) <= 0) fail("SSL_write failed");
    std::string response;
    char buffer[1024];
    int n = 0;
    while ((n = SSL_read(ssl, buffer, sizeof(buffer))) > 0) response.append(buffer, n);
    (void)SSL_shutdown(ssl);
    SSL_free(ssl);
    ::close(fd);
    SSL_CTX_free(ctx);

    HttpResponse parsed;
    auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) fail("malformed HTTP response");
    std::string headers = response.substr(0, header_end);
    parsed.body = response.substr(header_end + 4);
    std::istringstream header_stream(headers);
    std::string status_line;
    std::getline(header_stream, status_line);
    if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
    std::istringstream status_stream(status_line);
    std::string http_version;
    status_stream >> http_version >> parsed.status;
    return parsed;
}

int open_stalled_tcp_client(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) fail("unable to create stalled client socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        fail("unable to connect stalled client");
    }
    return fd;
}

std::string basic_auth_header(const std::string &user, const std::string &pass) {
    std::string token = user + ":" + pass;
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    uint32_t val = 0;
    int valb = -6;
    for (unsigned char c : token) {
        val = (val << 8U) | static_cast<uint32_t>(c);
        valb += 8;
        while (valb >= 0) {
            encoded.push_back(table[(val >> static_cast<unsigned int>(valb)) & 0x3FU]);
            valb -= 6;
        }
    }
    if (valb > -6) encoded.push_back(table[((val << 8U) >> static_cast<unsigned int>(valb + 8)) & 0x3FU]);
    while (encoded.size() % 4U) encoded.push_back('=');
    return std::string("Basic ") + encoded;
}

}  // namespace

int main() {
    StatsdCapture capture = start_statsd_capture();

    const char *cert_dir = "../tests/observability/certs/";
    std::string server_crt = std::string(cert_dir) + "server.crt";
    std::string server_key = std::string(cert_dir) + "server.key";
    std::string ca_crt = std::string(cert_dir) + "ca.crt";

    tsd_metrics_tls_config_t tls{};
    tls.certificate_path = server_crt.c_str();
    tls.private_key_path = server_key.c_str();
    tls.ca_certificate_path = ca_crt.c_str();

    tsd_metrics_basic_auth_t auth{};
    auth.username = "observer";
    auth.password = "secret";

    tsd_metrics_exporter_config_t config{};
    config.bind_address = "127.0.0.1";
    config.port = 0;
    config.tls = &tls;
    config.basic_auth = &auth;
    config.statsd_host = "127.0.0.1";
    config.statsd_port = capture.port;

    tsd_metrics_exporter_config_t rollback_config{};
    rollback_config.bind_address = "127.0.0.1";
    rollback_config.port = 0;
    (void)::setenv("TSD_TEST_METRICS_FAIL_THREAD_AFTER", "1", 1);
    if (tsd_metrics_exporter_start_with_config(&rollback_config) == 0) {
        fail("injected partial metrics startup unexpectedly succeeded");
    }
    if (tsd_metrics_exporter_listen_port() != 0) fail("failed metrics startup leaked listener state");
    (void)::unsetenv("TSD_TEST_METRICS_FAIL_THREAD_AFTER");

    if (tsd_metrics_exporter_start_with_config(&config) != 0) fail("metrics exporter failed to start");
    uint16_t port = tsd_metrics_exporter_listen_port();
    if (port == 0) fail("listen port not assigned");

    tsd_controller_telemetry_t controller{};
    controller.fallback_active = 0;
    controller.current_width = SIMD_AVX2;
    controller.recommended_width = SIMD_AVX2;
    tsd_observability_update_controller(&controller);

    tsd_fusion_telemetry_t fusion{};
    fusion.running = 1;
    fusion.degraded = 0;
    fusion.temp_available = 1;
    fusion.package_temp_c = 63.0;
    fusion.freq_available = 1;
    fusion.freq_ratio = 850.0;
    fusion.cpi_available = 1;
    fusion.thermal_cpi = 1.10;
    fusion.power_available = 1;
    fusion.power_budget_w = 75.0;
    tsd_observability_update_fusion(&fusion);

    tsd_temperature_channels_t channels{};
    channels.raw_available = 1;
    channels.raw_package_temp_c = 67.5;
    channels.filtered_available = 1;
    channels.filtered_package_temp_c = 63.0;
    tsd_observability_update_temperature_channels(&channels);

    tsd_perf_telemetry_t perf{};
    perf.mode = 1;
    perf.counters_healthy = 1;
    perf.pinned_cpu = 0;
    perf.monitor_cpu = 1;
    tsd_observability_update_perf(&perf);

    HttpResponse unauth = https_request(port, "/metrics", "", ca_crt);
    if (unauth.status != 401) fail("expected 401 for missing credentials");

    const std::string credentials = basic_auth_header("observer", "secret");
    HttpResponse metrics = https_request(port, "/metrics", credentials, ca_crt);
    if (metrics.status != 200 || metrics.body.find("tsd_patch_transitions_total") == std::string::npos ||
        metrics.body.find("channel=\"raw_safety\"") == std::string::npos ||
        metrics.body.find("channel=\"filtered_control\"") == std::string::npos) {
        fail("metrics response invalid, unescaped, or missing temperature channels");
    }

    HttpResponse health = https_request(port, "/healthz", credentials, ca_crt);
    if (health.status != 200 || health.body.find("\"live\":true") == std::string::npos ||
        health.body.find("\"perf\":") == std::string::npos ||
        health.body.find("\"rawTempAvailable\":true") == std::string::npos ||
        health.body.find("\"rawPackageTempC\":67.50") == std::string::npos ||
        health.body.find("\"filteredPackageTempC\":63.00") == std::string::npos) {
        fail("health response missing liveness/perf/raw-filtered temperature state");
    }
    HttpResponse ready = https_request(port, "/readyz", credentials, ca_crt);
    if (ready.status != 200) fail("expected ready response");

    /* A stalled TLS handshake may occupy one worker, but must not starve probes. */
    int stalled = open_stalled_tcp_client(port);
    auto probe = std::async(std::launch::async, [&] { return https_request(port, "/healthz", credentials, ca_crt); });
    if (probe.wait_for(std::chrono::milliseconds(1500)) != std::future_status::ready) {
        ::close(stalled);
        fail("stalled client starved health endpoint");
    }
    if (probe.get().status != 200) {
        ::close(stalled);
        fail("health endpoint failed while another client stalled");
    }
    ::close(stalled);

    perf.mode = 2;
    perf.counters_healthy = 0;
    tsd_observability_update_perf(&perf);
    HttpResponse degraded_ready = https_request(port, "/readyz", credentials, ca_crt);
    if (degraded_ready.status == 200) fail("software perf mode must not report ready");
    HttpResponse degraded_health = https_request(port, "/healthz", credentials, ca_crt);
    if (degraded_health.status != 200 || degraded_health.body.find("\"mode\":\"software\"") == std::string::npos) {
        fail("recoverable perf degradation must remain live and visible");
    }

    tsd_metrics_exporter_record_patch(SIMD_AVX2, SIMD_AVX512, 0, 5);
    if (capture.future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) fail("statsd emission missing");
    std::string statsd_payload = capture.future.get();
    if (statsd_payload.find("tsd.patch_transition.avx2.avx512.success") == std::string::npos) {
        fail("statsd payload missing transition");
    }

    tsd_metrics_exporter_record_sensor_health("pkg\"line\nslash\\sensor", 0, 1.0, 1.0, 1);
    HttpResponse escaped_metrics = https_request(port, "/metrics", credentials, ca_crt);
    if (escaped_metrics.status != 200 ||
        escaped_metrics.body.find("sensor=\"pkg\\\"line\\nslash\\\\sensor\"") == std::string::npos) {
        fail("Prometheus sensor label was not escaped");
    }

    tsd_metrics_exporter_stop();
    stop_statsd_capture(capture);
    return 0;
}
