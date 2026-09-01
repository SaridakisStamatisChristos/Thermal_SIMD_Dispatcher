#ifndef TSD_THERMAL_TRAMPOLINE_H
#define TSD_THERMAL_TRAMPOLINE_H

#include <pthread.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(__cplusplus)
#include <atomic>
#define TSD_ATOMIC_TYPE(T) std::atomic<T>
#else
#include <stdatomic.h>
#define TSD_ATOMIC_TYPE(T) _Atomic(T)
#endif

#include <thermal/simd/simd_width.h>

/**
 * Each indirect-call target is 64-byte aligned and begins with ENDBR64 so the
 * dispatcher is compatible with x86 CET Indirect Branch Tracking (IBT).
 * Alignment is a layout/cache property; CET shadow stacks do not require it.
 *
 * Production code slots are created once on a writable, non-executable page,
 * then the page is sealed read+execute before any slot can be published.
 * Runtime width changes select between immutable RX slots with an atomic
 * pointer update; executable code is never rewritten while the process runs.
 */
#if defined(__cplusplus)
struct alignas(64) tsd_patch_slot_t {
    uint8_t code[32];
};
#else
typedef struct {
    _Alignas(64) uint8_t code[32];
} tsd_patch_slot_t;
#endif

#define TSD_TRAMPOLINE_SLOT_SIZE 32U

typedef struct {
    tsd_patch_slot_t *active;
    tsd_patch_slot_t *inactive;
    void *page_a;
    void *page_b;
    size_t page_size;
    int page_a_prot;
    int page_b_prot;
    int pkey;
    bool has_pku;
    unsigned int pkru_write_mask;
    unsigned int pkru_disable_mask;
} tsd_trampoline_ctx_t;

#ifdef __cplusplus
extern "C" {
#endif

extern tsd_trampoline_ctx_t g_tsd_trampoline_ctx;
extern pthread_mutex_t g_tsd_patch_lock;
extern TSD_ATOMIC_TYPE(simd_width_t) g_tsd_current_width;
extern TSD_ATOMIC_TYPE(unsigned char) g_tsd_current_width_byte;
extern TSD_ATOMIC_TYPE(int) g_tsd_trampoline_initialized;
extern TSD_ATOMIC_TYPE(tsd_patch_slot_t*) g_tsd_active_trampoline;
extern TSD_ATOMIC_TYPE(unsigned char) g_tsd_last_patch_attempt;
extern TSD_ATOMIC_TYPE(unsigned char) g_tsd_last_patched_width;
extern TSD_ATOMIC_TYPE(bool) g_tsd_page_a_effective_writable;
extern TSD_ATOMIC_TYPE(bool) g_tsd_page_b_effective_writable;

int tsd_trampoline_init(void);

/*
 * Compatibility name retained for existing callers. This no longer rewrites
 * executable memory; it atomically selects an already-sealed implementation.
 */
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
int tsd_trampoline_test_last_window_used_pku(void);
#endif

#ifdef __cplusplus
}
#endif

#undef TSD_ATOMIC_TYPE

#endif /* TSD_THERMAL_TRAMPOLINE_H */
