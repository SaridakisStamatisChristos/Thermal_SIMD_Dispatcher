#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>

extern "C" {
#include <thermal/simd/statistics.h>
}

namespace {

double parse_env_double(const char *name, double fallback) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    try {
        return std::stod(value);
    } catch (const std::exception &) {
        std::cerr << "[perf_smoke] Ignoring invalid value for " << name << ": " << value
                  << std::endl;
        return fallback;
    }
}

std::uint64_t parse_env_uint64(const char *name, std::uint64_t fallback) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(value));
    } catch (const std::exception &) {
        std::cerr << "[perf_smoke] Ignoring invalid value for " << name << ": " << value
                  << std::endl;
        return fallback;
    }
}

std::uint64_t run_benchmark(std::uint64_t iterations) {
    std::mt19937 rng(1337u);
    std::uniform_int_distribution<std::uint32_t> dist(800, 1200);

    std::uint64_t ewma = 0;
    const unsigned ewma_shift = 3;

    auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        ewma = tsd_update_ewma(ewma, dist(rng), ewma_shift);
    }
    auto stop = std::chrono::steady_clock::now();

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
}

} // namespace

int main() {
    const std::uint64_t iterations = parse_env_uint64("TSD_PERF_ITERATIONS", 5'000'000ULL);
    const double threshold_ns = parse_env_double("TSD_PERF_THRESHOLD_NS", 300.0);

    if (iterations == 0) {
        std::cerr << "[perf_smoke] iteration count must be non-zero" << std::endl;
        return EXIT_FAILURE;
    }

    const std::uint64_t total_ns = run_benchmark(iterations);
    const double per_iteration = static_cast<double>(total_ns) / static_cast<double>(iterations);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "[perf_smoke] iterations=" << iterations << ", total_ns=" << total_ns
              << ", per_iteration_ns=" << per_iteration
              << ", threshold_ns=" << threshold_ns << std::endl;

    if (per_iteration > threshold_ns) {
        std::cerr << "[perf_smoke] performance regression detected: " << per_iteration
                  << " ns/iter exceeds threshold of " << threshold_ns << " ns/iter" << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
