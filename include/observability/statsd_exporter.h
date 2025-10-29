#pragma once

#include <cstdint>
#include <string>

#include <mutex>

#include <sys/socket.h>

namespace observability {

class StatsdExporter {
public:
    static StatsdExporter &instance();

    void configure(const std::string &host, uint16_t port);
    void shutdown();

    void send_counter(const std::string &name, uint64_t value);
    void send_gauge(const std::string &name, double value);

private:
    StatsdExporter();
    ~StatsdExporter();
    StatsdExporter(const StatsdExporter &) = delete;
    StatsdExporter &operator=(const StatsdExporter &) = delete;

    void send(const std::string &payload);

    int socket_;
    bool configured_;
    struct sockaddr_storage destination_;
    socklen_t destination_len_;
    std::mutex mutex_;
};

}  // namespace observability

