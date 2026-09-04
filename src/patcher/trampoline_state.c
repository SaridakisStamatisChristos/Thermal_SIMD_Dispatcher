#include <thermal/simd/thermal_trampoline.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>

_Atomic(simd_width_t) g_tsd_current_width = SIMD_SSE41;
_Atomic(unsigned char) g_tsd_current_width_byte = (unsigned char)SIMD_SSE41;
_Atomic(int) g_tsd_trampoline_initialized = 0;
_Atomic(tsd_patch_slot_t*) g_tsd_active_trampoline = NULL;
_Atomic(unsigned char) g_tsd_last_patch_attempt = (unsigned char)SIMD_SSE41;
_Atomic(unsigned char) g_tsd_last_patched_width = (unsigned char)SIMD_SSE41;
_Atomic(bool) g_tsd_page_a_effective_writable = false;
_Atomic(bool) g_tsd_page_b_effective_writable = false;

/* Slot pointers are 64-byte aligned, so the low two bits can encode width. */
static _Atomic(uintptr_t) g_tsd_selection_word = 0;
#define TSD_SELECTION_WIDTH_MASK ((uintptr_t)0x3u)

static uintptr_t encode_selection(simd_width_t width, tsd_patch_slot_t *active) {
    uintptr_t ptr = (uintptr_t)active;
    if (!active || (ptr & TSD_SELECTION_WIDTH_MASK) != 0 ||
        width < SIMD_SSE41 || width > SIMD_AVX512) {
        return 0;
    }
    return ptr | (uintptr_t)width;
}

int tsd_trampoline_state_snapshot(tsd_trampoline_selection_t *out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    uintptr_t word = atomic_load_explicit(&g_tsd_selection_word, memory_order_acquire);
    if (word == 0) {
        out->width = SIMD_SSE41;
        out->active = NULL;
        errno = ENODEV;
        return -1;
    }
    simd_width_t width = (simd_width_t)(word & TSD_SELECTION_WIDTH_MASK);
    tsd_patch_slot_t *active = (tsd_patch_slot_t *)(word & ~TSD_SELECTION_WIDTH_MASK);
    if (width < SIMD_SSE41 || width > SIMD_AVX512 || !active) {
        out->width = SIMD_SSE41;
        out->active = NULL;
        errno = EIO;
        return -1;
    }
    out->width = width;
    out->active = active;
    return 0;
}

simd_width_t tsd_trampoline_state_current_width(void) {
    tsd_trampoline_selection_t snapshot;
    if (tsd_trampoline_state_snapshot(&snapshot) == 0) return snapshot.width;
    return atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
}

unsigned char tsd_trampoline_state_current_width_byte(void) {
    return (unsigned char)tsd_trampoline_state_current_width();
}

int tsd_trampoline_state_initialized(void) {
    return atomic_load_explicit(&g_tsd_trampoline_initialized, memory_order_acquire);
}

tsd_patch_slot_t *tsd_trampoline_state_active(void) {
    tsd_trampoline_selection_t snapshot;
    if (tsd_trampoline_state_snapshot(&snapshot) == 0) return snapshot.active;
    return atomic_load_explicit(&g_tsd_active_trampoline, memory_order_acquire);
}

unsigned char tsd_trampoline_state_last_patch_attempt(void) {
    return atomic_load_explicit(&g_tsd_last_patch_attempt, memory_order_acquire);
}

unsigned char tsd_trampoline_state_last_patched_width(void) {
    return atomic_load_explicit(&g_tsd_last_patched_width, memory_order_acquire);
}

int tsd_trampoline_state_page_a_writable(void) {
    return atomic_load_explicit(&g_tsd_page_a_effective_writable, memory_order_acquire) ? 1 : 0;
}

int tsd_trampoline_state_page_b_writable(void) {
    return atomic_load_explicit(&g_tsd_page_b_effective_writable, memory_order_acquire) ? 1 : 0;
}

void tsd_trampoline_state_reset(tsd_patch_slot_t *active) {
    uintptr_t word = encode_selection(SIMD_SSE41, active);
    atomic_store_explicit(&g_tsd_selection_word, word, memory_order_release);
    atomic_store_explicit(&g_tsd_current_width, SIMD_SSE41, memory_order_release);
    atomic_store_explicit(&g_tsd_current_width_byte, (unsigned char)SIMD_SSE41, memory_order_release);
    atomic_store_explicit(&g_tsd_active_trampoline, active, memory_order_release);
    atomic_store_explicit(&g_tsd_trampoline_initialized, 0, memory_order_release);
    atomic_store_explicit(&g_tsd_last_patch_attempt, (unsigned char)SIMD_SSE41, memory_order_release);
    atomic_store_explicit(&g_tsd_last_patched_width, (unsigned char)SIMD_SSE41, memory_order_release);
}

void tsd_trampoline_state_set_initialized(int initialized) {
    atomic_store_explicit(&g_tsd_trampoline_initialized, initialized ? 1 : 0, memory_order_release);
}

void tsd_trampoline_state_set_last_patch_attempt(simd_width_t width) {
    atomic_store_explicit(&g_tsd_last_patch_attempt, (unsigned char)width, memory_order_release);
}

void tsd_trampoline_state_publish_selection(simd_width_t width, tsd_patch_slot_t *active) {
    uintptr_t word = encode_selection(width, active);
    if (word == 0) return;

    /* One atomic word is the authority for execution. Compatibility mirrors
     * are updated afterwards and are intentionally diagnostic-only. */
    atomic_store_explicit(&g_tsd_selection_word, word, memory_order_release);
    atomic_store_explicit(&g_tsd_current_width, width, memory_order_release);
    atomic_store_explicit(&g_tsd_current_width_byte, (unsigned char)width, memory_order_release);
    atomic_store_explicit(&g_tsd_active_trampoline, active, memory_order_release);
    atomic_store_explicit(&g_tsd_trampoline_initialized, 1, memory_order_release);
    atomic_store_explicit(&g_tsd_last_patched_width, (unsigned char)width, memory_order_release);
}

void tsd_trampoline_state_set_page_a_writable(int writable) {
    atomic_store_explicit(&g_tsd_page_a_effective_writable, writable != 0, memory_order_release);
}

void tsd_trampoline_state_set_page_b_writable(int writable) {
    atomic_store_explicit(&g_tsd_page_b_effective_writable, writable != 0, memory_order_release);
}
