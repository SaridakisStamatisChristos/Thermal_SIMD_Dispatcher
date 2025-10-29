#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <observability/metrics_exporter.h>
#include <tools/policy_tuner/tuner.h>

using tools::policy_tuner::LoadArchive;
using tools::policy_tuner::TuneFromSamples;
using tools::policy_tuner::TuningResult;
using tools::policy_tuner::WritePolicyBundle;

namespace {

std::string FetchMetrics(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    assert(rc == 0);
    (void)rc;

    const char request[] = "GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t sent = ::send(fd, request, sizeof(request) - 1, 0);
    assert(sent == static_cast<ssize_t>(sizeof(request) - 1));
    (void)sent;

    std::string response;
    char buffer[1024];
    while (true) {
        ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return response;
}

} // namespace

int main() {
    // Metrics exporter lifecycle
    int start_rc = tsd_metrics_exporter_start("127.0.0.1", 0);
    assert(start_rc == 0);
    (void)start_rc;
    uint16_t port = tsd_metrics_exporter_listen_port();
    assert(port != 0);

    tsd_metrics_exporter_record_patch(SIMD_SSE41, SIMD_AVX2, 0, 0);
    tsd_metrics_exporter_record_patch(SIMD_AVX2, SIMD_AVX512, -1, 0);
    tsd_metrics_exporter_observe_dwell(SIMD_SSE41, 250);
    tsd_metrics_exporter_record_sensor_health("hfi_pmt", 0, 0.85, 0.75, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::string response = FetchMetrics(port);
    assert(response.find("HTTP/1.1 200 OK") != std::string::npos);
    assert(response.find("tsd_patch_transitions_total{from=\"sse41\",to=\"avx2\",outcome=\"success\"}") !=
           std::string::npos);
    assert(response.find("tsd_patch_transitions_total{from=\"avx2\",to=\"avx512\",outcome=\"failure\"}") !=
           std::string::npos);
    assert(response.find("tsd_dwell_time_ms_sum{width=\"sse41\"} 250") != std::string::npos);
    assert(response.find("tsd_sensor_health_ratio{sensor=\"hfi_pmt\",socket=\"0\"} 0.85") != std::string::npos);

    tsd_metrics_exporter_stop();

    // Policy tuner workflow
    namespace fs = std::filesystem;
    fs::path tmpdir = fs::temp_directory_path() / "tsd_metrics_tuner_test";
    fs::create_directories(tmpdir);
    fs::path archive_path = tmpdir / "telemetry.csv";
    {
        std::ofstream out(archive_path);
        out << "# timestamp,socket,width,cpi_ratio,temp_c,dwell_ms\n";
        out << "0,0,sse41,1.30,65.0,200\n";
        out << "1,0,avx2,1.10,72.5,150\n";
        out << "2,1,avx512,1.85,80.0,420\n";
    }

    auto samples = LoadArchive(archive_path.string());
    assert(samples.size() == 3);

    TuningResult result = TuneFromSamples(samples);
    assert(result.sample_count == samples.size());
    assert(result.policy.slo_ratio_milli >= 900);
    assert(result.policy.slo_ratio_milli <= 4000);
    assert(result.policy.transition_penalty_down_milli >= result.policy.transition_penalty_up_milli);

    fs::path bundle_path = tmpdir / "policy_bundle.json";
    bool wrote = WritePolicyBundle(bundle_path.string(), result);
    assert(wrote);
    (void)wrote;

    std::ifstream in(bundle_path);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(contents.find("\"metadata\"") != std::string::npos);
    assert(contents.find("\"policy\"") != std::string::npos);
    assert(contents.find("\"forecast_horizon\"") != std::string::npos);

    fs::remove(bundle_path);
    fs::remove(archive_path);
    fs::remove(tmpdir);
    return 0;
}

