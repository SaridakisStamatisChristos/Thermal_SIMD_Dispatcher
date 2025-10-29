#ifndef TSD_HEALTHCHECK_SANDBOX_H
#define TSD_HEALTHCHECK_SANDBOX_H

#include <stddef.h>

#ifdef __cplusplus
#include <string>

namespace healthcheck {

struct SandboxResult {
    bool success{false};
    bool telemetry_ok{false};
    bool patch_ok{false};
    std::string message;
};

SandboxResult RunSandbox();

} // namespace healthcheck
#endif

#ifdef __cplusplus
extern "C" {
#endif

int tsd_sandbox_run(char *message, size_t message_len);

#ifdef __cplusplus
}
#endif

#endif /* TSD_HEALTHCHECK_SANDBOX_H */
