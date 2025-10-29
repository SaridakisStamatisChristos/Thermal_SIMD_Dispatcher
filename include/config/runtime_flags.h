#ifndef TSD_CONFIG_RUNTIME_FLAGS_H
#define TSD_CONFIG_RUNTIME_FLAGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void tsd_runtime_flags_init(void);
void tsd_runtime_flags_record_sandbox_success(void);
void tsd_runtime_flags_record_sandbox_failure(const char *message);
int tsd_runtime_flags_allow_transitions(void);
int tsd_runtime_flags_sandbox_complete(void);
const char* tsd_runtime_flags_status_message(void);
void tsd_runtime_flags_set_sandbox_only(int enabled);
int tsd_runtime_flags_sandbox_only(void);

#ifdef __cplusplus
}
#endif

#endif /* TSD_CONFIG_RUNTIME_FLAGS_H */
