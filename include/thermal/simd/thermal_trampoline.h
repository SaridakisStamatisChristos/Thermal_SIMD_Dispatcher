#ifndef TSD_THERMAL_TRAMPOLINE_H
#define TSD_THERMAL_TRAMPOLINE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include <thermal/simd/simd_width.h>

typedef struct {
    uint8_t code[16];
} tsd_patch_slot_t;

typedef struct {
    tsd_patch_slot_t *active;
    tsd_patch_slot_t *inactive;
    void *page_a;
    void *page_b;
    size_t page_size;
    int page_a_prot;
    int page_b_prot;
} tsd_trampoline_ctx_t;

extern tsd_trampoline_ctx_t g_tsd_trampoline_ctx;
extern pthread_mutex_t g_tsd_patch_lock;
extern _Atomic(simd_width_t) g_tsd_current_width;
extern _Atomic unsigned char g_tsd_current_width_byte;
extern _Atomic int g_tsd_trampoline_initialized;
extern _Atomic(tsd_patch_slot_t*) g_tsd_active_trampoline;
extern _Atomic unsigned char g_tsd_last_patch_attempt;
extern _Atomic unsigned char g_tsd_last_patched_width;

int tsd_trampoline_init(void);
int tsd_trampoline_patch(simd_width_t new_width);
int init_double_buffer_trampoline(void);
int tsd_trampoline_self_validate(char *reason, size_t reason_len);

#ifdef TSD_ENABLE_TESTS
void tsd_trampoline_override_patch(simd_width_t width, const uint8_t *bytes, size_t len);
void tsd_trampoline_clear_overrides(void);
const uint8_t* tsd_trampoline_patch_bytes(simd_width_t width, size_t *len);
void tsd_trampoline_force_failure(int stage);
const char* tsd_trampoline_last_error(void);
int tsd_trampoline_inactive_page_writable(void);
#endif

#endif /* TSD_THERMAL_TRAMPOLINE_H */
