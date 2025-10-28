#ifndef TSD_PATCHER_ATTESTATION_H
#define TSD_PATCHER_ATTESTATION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSD_ATTESTATION_HASH_SIZE 32U

int tsd_attestation_get_active_hash(uint8_t *buffer, size_t len);
int tsd_attestation_get_active_hash_hex(char *buffer, size_t len);
int tsd_attestation_expect_active_hash(const uint8_t *expected, size_t len);
const char* tsd_attestation_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* TSD_PATCHER_ATTESTATION_H */
