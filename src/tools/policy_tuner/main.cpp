#include <tools/policy_tuner/tuner.h>

#include <cstdio>
#include <iostream>
#include <string>

namespace {

void PrintUsage(const char *prog) {
    std::fprintf(stderr, "Usage: %s <telemetry-archive.csv> <output-bundle.json>\n", prog);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        PrintUsage(argc > 0 ? argv[0] : "policy_tuner");
        return 1;
    }

    std::string archive_path = argv[1];
    std::string output_path = argv[2];

    auto samples = tools::policy_tuner::LoadArchive(archive_path);
    if (samples.empty()) {
        std::cerr << "policy_tuner: no telemetry samples loaded from '" << archive_path << "'\n";
    }

    auto result = tools::policy_tuner::TuneFromSamples(samples);
    if (!tools::policy_tuner::WritePolicyBundle(output_path, result)) {
        std::cerr << "policy_tuner: failed to write policy bundle to '" << output_path << "'\n";
        return 1;
    }

    std::cout << "policy_tuner: wrote policy bundle to '" << output_path << "'";
    std::cout << " (samples=" << result.sample_count << ")\n";
    return 0;
}

