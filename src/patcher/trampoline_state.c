#include <thermal/simd/thermal_trampoline.h>

#include <stdatomic.h>

_Atomic(simd_width_t) g_tsd_current_width = SIMD_SSE41;
_Atomic(unsigned char) g_tsd_current_width_byte = (unsigned char)SIMD_SSE41;
_Atomic(int) g_tsd_trampoline_initialized = 0;
_Atomic(tsd_patch_slot_t*) g_tsd_active_trampoline = NULL;
_Atomic(unsigned char) g_tsd_last_patch_attempt = (unsigned char)SIMD_SSE41;
_Atomic(unsigned char) g_tsd_last_patched_width = (unsigned char)SIMD_SSE41;
_Atomic(bool) g_tsd_page_a_effective_writable = false;
_Atomic(bool) g_tsd_page_b_effective_writable = false;

simd_width_t tsd_trampoline_state_current_width(void) {
    return atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
}

unsigned char tsd_trampoline_state_current_width_byte(void) {
    return atomic_load_explicit(&g_tsd_current_width_byte, memory_order_acquire);
}

int tsd_trampoline_state_initialized(void) {
    return atomic_load_explicit(&g_tsd_trampoline_initialized, memory_order_acquire);
}

tsd_patch_slot_t *tsd_trampoline_state_active(void) {
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
    atomic_store_explicit(&g_tsd_current_width, SIMD_SSE41, memory_order_release);
    atomic_store_explicit(&g_tsd_current_width_byte, (unsigned char)SIMD_SSE41, memory_order_release);
    atomic_store_explicit(&g_tsd_active_trampoline, active, memory_order_seq_cst);
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
    /*
     * Publish compatibility width state before the active pointer, matching the
     * assembly shim's ordering assumptions. Control-plane readers that require
     * a coherent snapshot also take g_tsd_patch_lock.
     */
    atomic_store_explicit(&g_tsd_current_width, width, memory_order_release);
    atomic_store_explicit(&g_tsd_current_width_byte, (unsigned char)width, memory_order_release);
    atomic_store_explicit(&g_tsd_active_trampoline, active, memory_order_seq_cst);
    atomic_store_explicit(&g_tsd_trampoline_initialized, 1, memory_order_release);
    atomic_store_explicit(&g_tsd_last_patched_width, (unsigned char)width, memory_order_release);
}

void tsd_trampoline_state_set_page_a_writable(int writable) {
    atomic_store_explicit(&g_tsd_page_a_effective_writable, writable != 0, memory_order_release);
}

void tsd_trampoline_state_set_page_b_writable(int writable) {
    atomic_store_explicit(&g_tsd_page_b_effective_writable, writable != 0, memory_order_release);
}
