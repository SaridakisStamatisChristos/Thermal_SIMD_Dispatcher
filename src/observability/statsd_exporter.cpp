#include <observability/statsd_exporter.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <sstream>

namespace observability {

StatsdExporter &StatsdExporter::instance() {
    static StatsdExporter exporter;
    return exporter;
}

StatsdExporter::StatsdExporter() : socket_(-1), configured_(false), destination_{}, destination_len_(0) {}

StatsdExporter::~StatsdExporter() { shutdown(); }

void StatsdExporter::configure(const std::string &host, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    configured_ = false;
    destination_len_ = 0;

    if (host.empty() || port == 0) {
        return;
    }

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *result = nullptr;
    std::string port_string = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), port_string.c_str(), &hints, &result);
    if (rc != 0 || !result) {
        return;
    }

    socket_ = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socket_ < 0) {
        ::freeaddrinfo(result);
        return;
    }

    std::memset(&destination_, 0, sizeof(destination_));
    std::memcpy(&destination_, result->ai_addr, result->ai_addrlen);
    destination_len_ = static_cast<socklen_t>(result->ai_addrlen);
    configured_ = true;

    ::freeaddrinfo(result);
}

void StatsdExporter::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    configured_ = false;
    destination_len_ = 0;
}

void StatsdExporter::send_counter(const std::string &name, uint64_t value) {
    std::ostringstream stream;
    stream << name << ":" << value << "|c";
    send(stream.str());
}

void StatsdExporter::send_gauge(const std::string &name, double value) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(3);
    stream << name << ":" << value << "|g";
    send(stream.str());
}

void StatsdExporter::send(const std::string &payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || socket_ < 0) {
        return;
    }
    (void)::sendto(socket_, payload.data(), payload.size(), 0,
                    reinterpret_cast<sockaddr*>(&destination_), destination_len_);
}

}  // namespace observability

