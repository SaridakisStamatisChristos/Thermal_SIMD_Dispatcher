#include <patcher/attestation.h>

#include <thermal/simd/thermal_trampoline.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef TSD_ENABLE_TESTS
#define TEST_INTERNAL_ATOMIC_WRAP(T) std::atomic<T>
#define _Atomic(T) TEST_INTERNAL_ATOMIC_WRAP(T)
extern "C" {
#include "thermal_simd_test.h"
}
#undef _Atomic
#undef TEST_INTERNAL_ATOMIC_WRAP
#endif

static int assert_cet_alignment(void) {
    if (init_double_buffer_trampoline() != 0) {
        std::fprintf(stderr, "failed to initialise trampolines\n");
        return 1;
    }
    if (tsd_trampoline_patch(SIMD_SSE41) != 0) {
        std::fprintf(stderr, "failed to patch SSE4.1\n");
        return 1;
    }
    tsd_patch_slot_t *active = std::atomic_load_explicit(&g_tsd_active_trampoline, std::memory_order_acquire);
    if (!active) {
        std::fprintf(stderr, "active slot missing\n");
        return 1;
    }
    if ((reinterpret_cast<uintptr_t>(active) % 64) != 0) {
        std::fprintf(stderr, "active slot misaligned (addr=%p)\n", (void*)active);
        return 1;
    }
    const uint8_t *code = active->code;
    const uint8_t expected_prefix[4] = {0xF3, 0x0F, 0x1E, 0xFA};
    if (std::memcmp(code, expected_prefix, sizeof(expected_prefix)) != 0) {
        std::fprintf(stderr, "active slot missing ENDBR64 prefix\n");
        return 1;
    }
    size_t len = 0;
    const uint8_t *bytes = tsd_trampoline_patch_bytes(SIMD_SSE41, &len);
    if (!bytes || len != TSD_TRAMPOLINE_SLOT_SIZE) {
        std::fprintf(stderr, "unexpected trampoline slot size (%zu)\n", len);
        return 1;
    }
    return 0;
}

static int assert_pku_window_fallback(void) {
    tsd_trampoline_force_failure(TSD_PATCH_FAIL_PKU_WINDOW);
    if (tsd_trampoline_patch(SIMD_AVX2) != 0) {
        std::fprintf(stderr, "patch failed under PKU fallback\n");
        return 1;
    }
    int window_mode = tsd_trampoline_test_last_window_used_pku();
    if (window_mode == -1) {
        std::fprintf(stderr, "write window did not open\n");
        return 1;
    }
    if (g_tsd_trampoline_ctx.has_pku && window_mode != 0) {
        std::fprintf(stderr, "PKU fallback did not use mprotect (mode=%d)\n", window_mode);
        return 1;
    }
    if (tsd_trampoline_inactive_page_writable() != 0) {
        std::fprintf(stderr, "inactive page left writable\n");
        return 1;
    }
    return 0;
}

static int assert_attestation_alert(void) {
    if (tsd_trampoline_patch(SIMD_SSE41) != 0) {
        std::fprintf(stderr, "failed to refresh SSE4.1 patch\n");
        return 1;
    }
    uint8_t baseline[TSD_ATTESTATION_HASH_SIZE];
    if (tsd_attestation_get_active_hash(baseline, sizeof(baseline)) != 0) {
        std::fprintf(stderr, "unable to snapshot baseline hash: %s\n", tsd_attestation_last_error());
        return 1;
    }
    if (tsd_trampoline_patch(SIMD_AVX2) != 0) {
        std::fprintf(stderr, "failed to pivot to AVX2 before override\n");
        return 1;
    }
    uint8_t override_patch[TSD_TRAMPOLINE_SLOT_SIZE];
    size_t len = 0;
    const uint8_t *canonical = tsd_trampoline_patch_bytes(SIMD_SSE41, &len);
    if (!canonical || len != TSD_TRAMPOLINE_SLOT_SIZE) {
        std::fprintf(stderr, "failed to fetch canonical patch bytes\n");
        return 1;
    }
    std::memcpy(override_patch, canonical, len);
    override_patch[10] ^= 0xFFu;
    tsd_trampoline_override_patch(SIMD_SSE41, override_patch, len);
    if (tsd_trampoline_patch(SIMD_SSE41) != 0) {
        std::fprintf(stderr, "failed to install override patch\n");
        tsd_trampoline_clear_overrides();
        return 1;
    }
    if (tsd_attestation_expect_active_hash(baseline, sizeof(baseline)) == 0) {
        std::fprintf(stderr, "attestation accepted tampered hash\n");
        tsd_trampoline_clear_overrides();
        return 1;
    }
    const char *err = tsd_attestation_last_error();
    if (!err || std::strstr(err, "mismatch") == nullptr) {
        std::fprintf(stderr, "unexpected attestation error string: %s\n", err ? err : "<null>");
        tsd_trampoline_clear_overrides();
        return 1;
    }
    tsd_trampoline_clear_overrides();
    if (tsd_trampoline_patch(SIMD_SSE41) != 0) {
        std::fprintf(stderr, "failed to restore canonical patch\n");
        return 1;
    }
    return 0;
}

int main(void) {
    if (assert_cet_alignment() != 0) {
        return 1;
    }
    if (assert_pku_window_fallback() != 0) {
        return 1;
    }
    if (assert_attestation_alert() != 0) {
        return 1;
    }
    return 0;
}
