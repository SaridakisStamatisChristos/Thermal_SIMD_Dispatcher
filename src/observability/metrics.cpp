#include <observability/metrics_exporter.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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

class PrometheusExporter {
public:
    static PrometheusExporter &instance() {
        static PrometheusExporter exporter;
        return exporter;
    }

    int start(const std::string &bind_address, uint16_t port) {
        std::lock_guard<std::mutex> lock(server_mutex_);
        if (running_) {
            return -1;
        }

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        int opt = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (bind_address.empty()) {
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        } else {
            if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
                ::close(fd);
                return -1;
            }
        }
        addr.sin_port = htons(port);

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

        listen_port_ = ntohs(actual.sin_port);
        bind_address_ = bind_address.empty() ? std::string("127.0.0.1") : bind_address;
        running_ = true;
        listen_fd_ = fd;
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
    }

    uint16_t listen_port() const {
        std::lock_guard<std::mutex> lock(server_mutex_);
        return listen_port_;
    }

    void record_patch(simd_width_t from, simd_width_t to, int rc, uint64_t dwell_ms) {
        {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            PatchKey key{from, to, rc == 0};
            patch_counts_[key] += 1;
            if (dwell_ms > 0) {
                dwell_stats_[from].count += 1;
                dwell_stats_[from].total_ms += dwell_ms;
                dwell_stats_[from].max_ms = std::max(dwell_stats_[from].max_ms, dwell_ms);
            }
        }
    }

    void observe_dwell(simd_width_t width, uint64_t dwell_ms) {
        if (dwell_ms == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        DwellStats &stats = dwell_stats_[width];
        stats.count += 1;
        stats.total_ms += dwell_ms;
        stats.max_ms = std::max(stats.max_ms, dwell_ms);
    }

    void record_sensor_health(const std::string &sensor,
                              int socket,
                              double health,
                              double quality,
                              bool valid) {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        SensorKey key{sensor, socket};
        SensorSnapshot snapshot;
        snapshot.health = std::clamp(health, 0.0, 1.0);
        snapshot.quality = std::clamp(quality, 0.0, 1.0);
        snapshot.valid = valid;
        snapshot.updated_at = Clock::now();
        sensor_state_[key] = snapshot;
    }

    std::string build_metrics() {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
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
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                entry.second.updated_at.time_since_epoch());
            body << "tsd_sensor_health_timestamp_seconds{sensor=\"" << entry.first.sensor
                 << "\",socket=\"" << entry.first.socket << "\"} "
                 << secs.count() << "\n";
        }

        return body.str();
    }

private:
    PrometheusExporter() = default;
    ~PrometheusExporter() { stop(); }
    PrometheusExporter(const PrometheusExporter &) = delete;
    PrometheusExporter &operator=(const PrometheusExporter &) = delete;

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
            handle_client(client);
            ::close(client);
        }
    }

    void handle_client(int client) {
        char buffer[512];
        ssize_t n = ::recv(client, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            return;
        }
        buffer[n] = '\0';
        std::string request(buffer);
        if (request.rfind("GET /metrics", 0) != 0) {
            static const char kNotFound[] =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)::send(client, kNotFound, sizeof(kNotFound) - 1, 0);
            return;
        }

        std::string body = build_metrics();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: text/plain; version=0.0.4\r\n";
        response << "Content-Length: " << body.size() << "\r\n";
        response << "Cache-Control: no-cache\r\n";
        response << "Connection: close\r\n\r\n";
        response << body;
        const std::string &payload = response.str();
        size_t total_sent = 0;
        while (total_sent < payload.size()) {
            ssize_t sent = ::send(client, payload.data() + total_sent, payload.size() - total_sent, 0);
            if (sent <= 0) {
                break;
            }
            total_sent += static_cast<size_t>(sent);
        }
    }

    mutable std::mutex server_mutex_;
    mutable std::mutex metrics_mutex_;
    std::thread server_thread_;
    int listen_fd_{-1};
    uint16_t listen_port_{0};
    std::string bind_address_;
    bool running_{false};
    std::map<PatchKey, uint64_t> patch_counts_;
    std::map<simd_width_t, DwellStats> dwell_stats_;
    std::map<SensorKey, SensorSnapshot> sensor_state_;
};

} // namespace

extern "C" {

int tsd_metrics_exporter_start(const char *bind_address, uint16_t port) {
    std::string address = bind_address ? std::string(bind_address) : std::string();
    return PrometheusExporter::instance().start(address, port);
}

void tsd_metrics_exporter_stop(void) {
    PrometheusExporter::instance().stop();
}

uint16_t tsd_metrics_exporter_listen_port(void) {
    return PrometheusExporter::instance().listen_port();
}

void tsd_metrics_exporter_record_patch(simd_width_t from, simd_width_t to, int rc, uint64_t dwell_ms) {
    PrometheusExporter::instance().record_patch(from, to, rc, dwell_ms);
}

void tsd_metrics_exporter_observe_dwell(simd_width_t width, uint64_t dwell_ms) {
    PrometheusExporter::instance().observe_dwell(width, dwell_ms);
}

void tsd_metrics_exporter_record_sensor_health(const char *sensor_name,
                                               int socket,
                                               double health,
                                               double quality,
                                               int valid) {
    if (!sensor_name) {
        return;
    }
    PrometheusExporter::instance().record_sensor_health(sensor_name, socket, health, quality, valid != 0);
}

} // extern "C"

