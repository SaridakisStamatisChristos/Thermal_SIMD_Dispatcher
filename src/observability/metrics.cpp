#include <observability/metrics_exporter.h>

#include <observability/statsd_exporter.h>
#include <observability/telemetry_state.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::system_clock;

const char *width_to_string(simd_width_t width) {
    switch (width) {
        case SIMD_SSE41:
            return "sse41";
        case SIMD_AVX2:
            return "avx2";
        case SIMD_AVX512:
            return "avx512";
        default:
            return "unknown";
    }
}

std::string sanitize_for_statsd(const std::string &value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_') {
            result.push_back(ch);
        } else {
            result.push_back('_');
        }
    }
    return result;
}

struct PatchKey {
    simd_width_t from;
    simd_width_t to;
    bool success;

    bool operator<(const PatchKey &other) const {
        if (from != other.from) {
            return from < other.from;
        }
        if (to != other.to) {
            return to < other.to;
        }
        return success < other.success;
    }
};

struct DwellStats {
    uint64_t count{0};
    uint64_t total_ms{0};
    uint64_t max_ms{0};
};

struct SensorKey {
    std::string sensor;
    int socket{0};

    bool operator<(const SensorKey &other) const {
        if (sensor != other.sensor) {
            return sensor < other.sensor;
        }
        return socket < other.socket;
    }
};

struct SensorSnapshot {
    double health{0.0};
    double quality{0.0};
    bool valid{false};
    Clock::time_point updated_at{Clock::now()};
};

class MetricsRegistry {
public:
    static MetricsRegistry &instance() {
        static MetricsRegistry registry;
        return registry;
    }

    void record_patch(simd_width_t from, simd_width_t to, int rc, uint64_t dwell_ms) {
        const bool success = (rc == 0);
        std::string statsd_name = std::string("tsd.patch_transition.") + width_to_string(from) + "." + width_to_string(to) + (success ? ".success" : ".failure");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            PatchKey key{from, to, success};
            patch_counts_[key] += 1;
            if (dwell_ms > 0) {
                DwellStats &stats = dwell_stats_[from];
                stats.count += 1;
                stats.total_ms += dwell_ms;
                stats.max_ms = std::max(stats.max_ms, dwell_ms);
            }
        }
        observability::StatsdExporter::instance().send_counter(statsd_name, 1);
        if (dwell_ms > 0) {
            std::string dwell_metric = std::string("tsd.dwell.observed.") + width_to_string(from);
            observability::StatsdExporter::instance().send_gauge(dwell_metric, static_cast<double>(dwell_ms));
        }
    }

    void observe_dwell(simd_width_t width, uint64_t dwell_ms) {
        if (dwell_ms == 0) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            DwellStats &stats = dwell_stats_[width];
            stats.count += 1;
            stats.total_ms += dwell_ms;
            stats.max_ms = std::max(stats.max_ms, dwell_ms);
        }
        std::string dwell_metric = std::string("tsd.dwell_time.") + width_to_string(width);
        observability::StatsdExporter::instance().send_gauge(dwell_metric, static_cast<double>(dwell_ms));
    }

    void record_sensor_health(const std::string &sensor, int socket, double health, double quality, bool valid) {
        SensorKey key{sensor, socket};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            SensorSnapshot snapshot;
            snapshot.health = std::clamp(health, 0.0, 1.0);
            snapshot.quality = std::clamp(quality, 0.0, 1.0);
            snapshot.valid = valid;
            snapshot.updated_at = Clock::now();
            sensor_state_[key] = snapshot;
        }
        std::string base = std::string("tsd.sensor.") + sanitize_for_statsd(sensor) + ".socket" + std::to_string(socket);
        observability::StatsdExporter::instance().send_gauge(base + ".health", health);
        observability::StatsdExporter::instance().send_gauge(base + ".quality", quality);
        observability::StatsdExporter::instance().send_gauge(base + ".valid", valid ? 1.0 : 0.0);
    }

    std::string build_prometheus() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream body;

        body << "# HELP tsd_patch_transitions_total Dispatcher patch attempts by outcome.\n";
        body << "# TYPE tsd_patch_transitions_total counter\n";
        for (const auto &entry : patch_counts_) {
            const PatchKey &key = entry.first;
            body << "tsd_patch_transitions_total{from=\"" << width_to_string(key.from)
                 << "\",to=\"" << width_to_string(key.to) << "\",outcome=\""
                 << (key.success ? "success" : "failure") << "\"} " << entry.second << "\n";
        }

        body << "# HELP tsd_dwell_time_ms_sum Total dwell time observed before transitions.\n";
        body << "# TYPE tsd_dwell_time_ms_sum counter\n";
        for (const auto &entry : dwell_stats_) {
            body << "tsd_dwell_time_ms_sum{width=\"" << width_to_string(entry.first)
                 << "\"} " << entry.second.total_ms << "\n";
        }

        body << "# HELP tsd_dwell_time_ms_count Number of dwell observations.\n";
        body << "# TYPE tsd_dwell_time_ms_count counter\n";
        for (const auto &entry : dwell_stats_) {
            body << "tsd_dwell_time_ms_count{width=\"" << width_to_string(entry.first)
                 << "\"} " << entry.second.count << "\n";
        }

        body << "# HELP tsd_dwell_time_ms_max Maximum dwell time observed.\n";
        body << "# TYPE tsd_dwell_time_ms_max gauge\n";
        for (const auto &entry : dwell_stats_) {
            body << "tsd_dwell_time_ms_max{width=\"" << width_to_string(entry.first)
                 << "\"} " << entry.second.max_ms << "\n";
        }

        body << "# HELP tsd_sensor_health_ratio Last reported sensor health ratio.\n";
        body << "# TYPE tsd_sensor_health_ratio gauge\n";
        for (const auto &entry : sensor_state_) {
            body << "tsd_sensor_health_ratio{sensor=\"" << entry.first.sensor
                 << "\",socket=\"" << entry.first.socket << "\"} "
                 << entry.second.health << "\n";
        }

        body << "# HELP tsd_sensor_quality_ratio Last reported sensor quality ratio.\n";
        body << "# TYPE tsd_sensor_quality_ratio gauge\n";
        for (const auto &entry : sensor_state_) {
            body << "tsd_sensor_quality_ratio{sensor=\"" << entry.first.sensor
                 << "\",socket=\"" << entry.first.socket << "\"} "
                 << entry.second.quality << "\n";
        }

        body << "# HELP tsd_sensor_health_valid Last reported sensor validity (1=valid).\n";
        body << "# TYPE tsd_sensor_health_valid gauge\n";
        for (const auto &entry : sensor_state_) {
            body << "tsd_sensor_health_valid{sensor=\"" << entry.first.sensor
                 << "\",socket=\"" << entry.first.socket << "\"} "
                 << (entry.second.valid ? 1 : 0) << "\n";
        }

        body << "# HELP tsd_sensor_health_timestamp_seconds UNIX time of last sensor report.\n";
        body << "# TYPE tsd_sensor_health_timestamp_seconds gauge\n";
        for (const auto &entry : sensor_state_) {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(entry.second.updated_at.time_since_epoch());
            body << "tsd_sensor_health_timestamp_seconds{sensor=\"" << entry.first.sensor
                 << "\",socket=\"" << entry.first.socket << "\"} "
                 << secs.count() << "\n";
        }

        return body.str();
    }

private:
    MetricsRegistry() = default;

    std::mutex mutex_;
    std::map<PatchKey, uint64_t> patch_counts_;
    std::map<simd_width_t, DwellStats> dwell_stats_;
    std::map<SensorKey, SensorSnapshot> sensor_state_;
};

struct ExporterTlsConfig {
    bool enabled{false};
    std::string certificate;
    std::string key;
    std::string ca;
    bool require_client_auth{false};
};

struct ExporterBasicAuth {
    bool enabled{false};
    std::string username;
    std::string password;
};

struct ExporterConfig {
    std::string bind_address{"127.0.0.1"};
    uint16_t port{0};
    ExporterTlsConfig tls;
    ExporterBasicAuth auth;
    std::string statsd_host;
    uint16_t statsd_port{0};
};

class PrometheusExporter {
public:
    static PrometheusExporter &instance() {
        static PrometheusExporter exporter;
        return exporter;
    }

    int start(const ExporterConfig &config) {
        std::lock_guard<std::mutex> lock(server_mutex_);
        if (running_) {
            return -1;
        }

        ExporterConfig local_config = config;
        if (local_config.bind_address.empty()) {
            local_config.bind_address = "127.0.0.1";
        }

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        int opt = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (::inet_pton(AF_INET, local_config.bind_address.c_str(), &addr.sin_addr) != 1) {
            ::close(fd);
            return -1;
        }
        addr.sin_port = htons(local_config.port);

        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return -1;
        }
        if (::listen(fd, 16) != 0) {
            ::close(fd);
            return -1;
        }

        sockaddr_in actual{};
        socklen_t actual_len = sizeof(actual);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&actual), &actual_len) != 0) {
            ::close(fd);
            return -1;
        }

        if (!initialize_tls(local_config.tls)) {
            ::close(fd);
            return -1;
        }

        observability::StatsdExporter::instance().configure(local_config.statsd_host, local_config.statsd_port);

        listen_fd_ = fd;
        listen_port_ = ntohs(actual.sin_port);
        bind_address_ = local_config.bind_address;
        config_ = local_config;
        running_ = true;
        server_thread_ = std::thread(&PrometheusExporter::serve, this);
        return 0;
    }

    void stop() {
        std::thread local_thread;
        {
            std::lock_guard<std::mutex> lock(server_mutex_);
            if (!running_) {
                return;
            }
            running_ = false;
            observability::StatsdExporter::instance().shutdown();
            if (listen_fd_ >= 0) {
                ::shutdown(listen_fd_, SHUT_RDWR);
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
            local_thread = std::move(server_thread_);
        }
        if (local_thread.joinable()) {
            local_thread.join();
        }
        destroy_tls();
    }

    uint16_t listen_port() const {
        std::lock_guard<std::mutex> lock(server_mutex_);
        return listen_port_;
    }

    MetricsRegistry &registry() { return MetricsRegistry::instance(); }

private:
    PrometheusExporter() = default;
    ~PrometheusExporter() { stop(); }
    PrometheusExporter(const PrometheusExporter &) = delete;
    PrometheusExporter &operator=(const PrometheusExporter &) = delete;

    bool initialize_tls(const ExporterTlsConfig &tls) {
        if (!tls.enabled) {
            destroy_tls();
            use_tls_ = false;
            return true;
        }

        static std::once_flag init_once;
        std::call_once(init_once, [] { SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms(); });

        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx) {
            return false;
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate_file(ctx, tls.certificate.c_str(), SSL_FILETYPE_PEM) <= 0) {
            SSL_CTX_free(ctx);
            return false;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, tls.key.c_str(), SSL_FILETYPE_PEM) <= 0) {
            SSL_CTX_free(ctx);
            return false;
        }
        if (!tls.ca.empty()) {
            if (SSL_CTX_load_verify_locations(ctx, tls.ca.c_str(), nullptr) <= 0) {
                SSL_CTX_free(ctx);
                return false;
            }
        }
        if (tls.require_client_auth) {
            if (tls.ca.empty()) {
                SSL_CTX_free(ctx);
                return false;
            }
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }

        destroy_tls();
        ssl_ctx_ = ctx;
        use_tls_ = true;
        return true;
    }

    void destroy_tls() {
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
    }

    static std::string base64_decode(const std::string &input) {
        static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string output;
        output.reserve((input.size() * 3) / 4);
        int val = 0;
        int valb = -8;
        for (unsigned char c : input) {
            if (std::isspace(c)) {
                continue;
            }
            if (c == '=') {
                break;
            }
            auto pos = alphabet.find(c);
            if (pos == std::string::npos) {
                return std::string();
            }
            val = (val << 6) + static_cast<int>(pos);
            valb += 6;
            if (valb >= 0) {
                output.push_back(static_cast<char>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return output;
    }

    struct HttpRequest {
        std::string method;
        std::string path;
        std::map<std::string, std::string> headers;
    };

    static std::optional<HttpRequest> parse_request(const std::string &buffer) {
        HttpRequest request;
        std::istringstream stream(buffer);
        std::string line;
        if (!std::getline(stream, line)) {
            return std::nullopt;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream first(line);
        if (!(first >> request.method >> request.path)) {
            return std::nullopt;
        }
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                break;
            }
            auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
                value.erase(value.begin());
            }
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            request.headers[key] = value;
        }
        return request;
    }

    static bool send_payload(int client, SSL *ssl, const std::string &payload) {
        const char *data = payload.data();
        size_t remaining = payload.size();
        while (remaining > 0) {
            ssize_t sent = ssl ? SSL_write(ssl, data, static_cast<int>(remaining)) : ::send(client, data, remaining, 0);
            if (sent <= 0) {
                return false;
            }
            data += static_cast<size_t>(sent);
            remaining -= static_cast<size_t>(sent);
        }
        return true;
    }

    static std::string read_request_payload(int client, SSL *ssl) {
        std::string buffer;
        buffer.reserve(1024);
        char chunk[512];
        while (buffer.find("\r\n\r\n") == std::string::npos) {
            ssize_t received = ssl ? SSL_read(ssl, chunk, sizeof(chunk)) : ::recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<size_t>(received));
            if (buffer.size() > 8192) {
                break;
            }
        }
        return buffer;
    }

    bool authorized(const HttpRequest &request) const {
        if (!config_.auth.enabled) {
            return true;
        }
        auto it = request.headers.find("authorization");
        if (it == request.headers.end()) {
            return false;
        }
        const std::string &value = it->second;
        const std::string prefix = "Basic ";
        if (value.size() <= prefix.size() || value.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        std::string decoded = base64_decode(value.substr(prefix.size()));
        std::string expected = config_.auth.username + ":" + config_.auth.password;
        return decoded == expected;
    }

    std::string build_health_json() const {
        auto controller = observability::TelemetryState::instance().controller_snapshot();
        auto fusion = observability::TelemetryState::instance().fusion_snapshot();
        auto controller_secs = std::chrono::duration_cast<std::chrono::seconds>(controller.updated_at.time_since_epoch()).count();
        auto fusion_secs = std::chrono::duration_cast<std::chrono::seconds>(fusion.updated_at.time_since_epoch()).count();

        std::ostringstream json;
        json << "{\"controller\":{\"fallbackActive\":" << (controller.fallback_active ? "true" : "false")
             << ",\"currentWidth\":\"" << width_to_string(controller.current_width) << "\""
             << ",\"recommendedWidth\":\"" << width_to_string(controller.recommended_width) << "\""
             << ",\"issuedChange\":" << (controller.issued_change ? "true" : "false")
             << ",\"updatedAtSeconds\":" << controller_secs << "},"
             << "\"fusion\":{\"running\":" << (fusion.running ? "true" : "false")
             << ",\"degraded\":" << (fusion.degraded ? "true" : "false")
             << ",\"tempAvailable\":" << (fusion.temp_available ? "true" : "false")
             << ",\"packageTempC\":" << std::fixed << std::setprecision(2) << fusion.package_temp_c
             << ",\"freqAvailable\":" << (fusion.freq_available ? "true" : "false")
             << ",\"freqRatio\":" << fusion.freq_ratio
             << ",\"cpiAvailable\":" << (fusion.cpi_available ? "true" : "false")
             << ",\"thermalCpi\":" << fusion.thermal_cpi
             << ",\"powerAvailable\":" << (fusion.power_available ? "true" : "false")
             << ",\"powerBudgetW\":" << fusion.power_budget_w
             << ",\"updatedAtSeconds\":" << fusion_secs << "}}";
        return json.str();
    }

    bool readiness_ok() const {
        auto controller = observability::TelemetryState::instance().controller_snapshot();
        auto fusion = observability::TelemetryState::instance().fusion_snapshot();
        auto now = std::chrono::system_clock::now();
        bool controller_recent = controller.updated_at.time_since_epoch().count() != 0 &&
                                 (now - controller.updated_at) <= std::chrono::seconds(5);
        bool fusion_recent = fusion.updated_at.time_since_epoch().count() != 0 &&
                              (now - fusion.updated_at) <= std::chrono::seconds(5);
        bool healthy = controller_recent && fusion_recent && !controller.fallback_active && fusion.running && !fusion.degraded;
        return healthy;
    }

    bool health_ok() const {
        auto controller = observability::TelemetryState::instance().controller_snapshot();
        auto fusion = observability::TelemetryState::instance().fusion_snapshot();
        return !controller.fallback_active && fusion.running && !fusion.degraded;
    }

    static std::string build_response(const std::string &status,
                                      const std::string &content_type,
                                      const std::string &body,
                                      const std::vector<std::pair<std::string, std::string>> &headers = {}) {
        std::ostringstream response;
        response << "HTTP/1.1 " << status << "\r\n";
        response << "Content-Type: " << content_type << "\r\n";
        response << "Content-Length: " << body.size() << "\r\n";
        response << "Cache-Control: no-cache\r\n";
        for (const auto &header : headers) {
            response << header.first << ": " << header.second << "\r\n";
        }
        response << "Connection: close\r\n\r\n";
        response << body;
        return response.str();
    }

    void serve() {
        while (true) {
            int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) {
                std::lock_guard<std::mutex> lock(server_mutex_);
                if (!running_) {
                    break;
                }
                continue;
            }
            if (use_tls_ && ssl_ctx_) {
                SSL *ssl = SSL_new(ssl_ctx_);
                if (!ssl) {
                    ::close(client);
                    continue;
                }
                SSL_set_fd(ssl, client);
                if (SSL_accept(ssl) <= 0) {
                    SSL_free(ssl);
                    ::close(client);
                    continue;
                }
                handle_client(client, ssl);
                SSL_shutdown(ssl);
                SSL_free(ssl);
                ::close(client);
            } else {
                handle_client(client, nullptr);
                ::close(client);
            }
        }
    }

    void handle_client(int client, SSL *ssl) {
        std::string request_payload = read_request_payload(client, ssl);
        if (request_payload.empty()) {
            return;
        }
        auto parsed = parse_request(request_payload);
        if (!parsed) {
            std::string response = build_response("400 Bad Request", "text/plain", "bad request\n");
            send_payload(client, ssl, response);
            return;
        }
        const HttpRequest &request = *parsed;
        if (request.method != "GET") {
            std::string response = build_response("405 Method Not Allowed", "text/plain", "method not allowed\n");
            send_payload(client, ssl, response);
            return;
        }

        if (!authorized(request)) {
            std::vector<std::pair<std::string, std::string>> headers = {{"WWW-Authenticate", "Basic realm=\"metrics\""}};
            std::string response = build_response("401 Unauthorized", "text/plain", "unauthorized\n", headers);
            send_payload(client, ssl, response);
            return;
        }

        if (request.path == "/metrics") {
            std::string body = MetricsRegistry::instance().build_prometheus();
            std::string response = build_response("200 OK", "text/plain; version=0.0.4", body);
            send_payload(client, ssl, response);
            return;
        }

        if (request.path == "/healthz") {
            bool ok = health_ok();
            std::string body = build_health_json();
            std::string response = build_response(ok ? "200 OK" : "503 Service Unavailable", "application/json", body);
            send_payload(client, ssl, response);
            return;
        }

        if (request.path == "/readyz") {
            bool ok = readiness_ok();
            std::string body = build_health_json();
            std::string response = build_response(ok ? "200 OK" : "503 Service Unavailable", "application/json", body);
            send_payload(client, ssl, response);
            return;
        }

        std::string response = build_response("404 Not Found", "text/plain", "not found\n");
        send_payload(client, ssl, response);
    }

    mutable std::mutex server_mutex_;
    std::thread server_thread_;
    bool running_{false};
    int listen_fd_{-1};
    uint16_t listen_port_{0};
    std::string bind_address_{"127.0.0.1"};
    ExporterConfig config_{};
    bool use_tls_{false};
    SSL_CTX *ssl_ctx_{nullptr};
};

} // namespace

extern "C" {

int tsd_metrics_exporter_start_with_config(const tsd_metrics_exporter_config_t *config) {
    ExporterConfig exporter_config;
    if (config) {
        if (config->bind_address) {
            exporter_config.bind_address = config->bind_address;
        }
        exporter_config.port = config->port;
        if (config->tls) {
            exporter_config.tls.enabled = true;
            if (config->tls->certificate_path) {
                exporter_config.tls.certificate = config->tls->certificate_path;
            }
            if (config->tls->private_key_path) {
                exporter_config.tls.key = config->tls->private_key_path;
            }
            if (config->tls->ca_certificate_path) {
                exporter_config.tls.ca = config->tls->ca_certificate_path;
            }
            exporter_config.tls.require_client_auth = config->tls->require_client_auth != 0;
        }
        if (config->basic_auth) {
            exporter_config.auth.enabled = true;
            if (config->basic_auth->username) {
                exporter_config.auth.username = config->basic_auth->username;
            }
            if (config->basic_auth->password) {
                exporter_config.auth.password = config->basic_auth->password;
            }
        }
        if (config->statsd_host) {
            exporter_config.statsd_host = config->statsd_host;
        }
        exporter_config.statsd_port = config->statsd_port;
    }
    return PrometheusExporter::instance().start(exporter_config);
}

int tsd_metrics_exporter_start(const char *bind_address, uint16_t port) {
    tsd_metrics_exporter_config_t config{};
    config.bind_address = bind_address;
    config.port = port;
    return tsd_metrics_exporter_start_with_config(&config);
}

void tsd_metrics_exporter_stop(void) {
    PrometheusExporter::instance().stop();
}

uint16_t tsd_metrics_exporter_listen_port(void) {
    return PrometheusExporter::instance().listen_port();
}

void tsd_metrics_exporter_record_patch(simd_width_t from, simd_width_t to, int rc, uint64_t dwell_ms) {
    PrometheusExporter::instance().registry().record_patch(from, to, rc, dwell_ms);
}

void tsd_metrics_exporter_observe_dwell(simd_width_t width, uint64_t dwell_ms) {
    PrometheusExporter::instance().registry().observe_dwell(width, dwell_ms);
}

void tsd_metrics_exporter_record_sensor_health(const char *sensor_name,
                                               int socket,
                                               double health,
                                               double quality,
                                               int valid) {
    if (!sensor_name) {
        return;
    }
    PrometheusExporter::instance().registry().record_sensor_health(sensor_name, socket, health, quality, valid != 0);
}

} // extern "C"

