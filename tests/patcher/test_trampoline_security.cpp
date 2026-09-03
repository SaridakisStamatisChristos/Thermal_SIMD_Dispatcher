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

static int mapping_is_rx_not_writable(const void *address) {
    if (!address) {
        return 0;
    }
    FILE *maps = std::fopen("/proc/self/maps", "r");
    if (!maps) {
        std::fprintf(stderr, "unable to open /proc/self/maps\n");
        return 0;
    }
    uintptr_t needle = reinterpret_cast<uintptr_t>(address);
    char line[512];
    int ok = 0;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char perms[5] = {0};
        if (std::sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3) {
            continue;
        }
        if (needle >= start && needle < end) {
            ok = perms[0] == 'r' && perms[1] != 'w' && perms[2] == 'x';
            break;
        }
    }
    std::fclose(maps);
    return ok;
}

static int contains_sequence(const uint8_t *bytes, size_t len, const uint8_t *needle, size_t needle_len) {
    if (!bytes || !needle || needle_len == 0 || needle_len > len) {
        return 0;
    }
    for (size_t i = 0; i + needle_len <= len; ++i) {
        if (std::memcmp(bytes + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int assert_immutable_cet_table(void) {
    if (init_double_buffer_trampoline() != 0) {
        std::fprintf(stderr, "failed to initialise trampoline table\n");
        return 1;
    }
    if (tsd_trampoline_patch(SIMD_SSE41) != 0) {
        std::fprintf(stderr, "failed to select SSE4.1\n");
        return 1;
    }

    tsd_patch_slot_t *active = tsd_trampoline_state_active();
    if (!active) {
        std::fprintf(stderr, "active slot missing\n");
        return 1;
    }
    if ((reinterpret_cast<uintptr_t>(active) % 64) != 0) {
        std::fprintf(stderr, "active slot misaligned (addr=%p)\n", (void*)active);
        return 1;
    }
    const uint8_t expected_prefix[4] = {0xF3, 0x0F, 0x1E, 0xFA};
    if (std::memcmp(active->code, expected_prefix, sizeof(expected_prefix)) != 0) {
        std::fprintf(stderr, "active slot missing ENDBR64 prefix\n");
        return 1;
    }
    if (!mapping_is_rx_not_writable(active)) {
        std::fprintf(stderr, "active trampoline mapping is not RX-only\n");
        return 1;
    }
    if (tsd_trampoline_inactive_page_writable() != 0) {
        std::fprintf(stderr, "inactive trampoline unexpectedly writable\n");
        return 1;
    }

    char reason[256] = {0};
    if (tsd_trampoline_self_validate(reason, sizeof(reason)) != 0) {
        std::fprintf(stderr, "self validation failed: %s\n", reason);
        return 1;
    }
    return 0;
}

static int assert_native_vector_width_payloads(void) {
    size_t sse_len = 0;
    size_t avx2_len = 0;
    size_t avx512_len = 0;
    const uint8_t *sse = tsd_trampoline_patch_bytes(SIMD_SSE41, &sse_len);
    const uint8_t *avx2 = tsd_trampoline_patch_bytes(SIMD_AVX2, &avx2_len);
    const uint8_t *avx512 = tsd_trampoline_patch_bytes(SIMD_AVX512, &avx512_len);
    if (!sse || !avx2 || !avx512 ||
        sse_len != TSD_TRAMPOLINE_SLOT_SIZE ||
        avx2_len != TSD_TRAMPOLINE_SLOT_SIZE ||
        avx512_len != TSD_TRAMPOLINE_SLOT_SIZE) {
        std::fprintf(stderr, "unexpected trampoline payload size\n");
        return 1;
    }

    /* pshufd xmm0,xmm0,0 establishes an explicit 128-bit SSE lane fill. */
    const uint8_t sse_broadcast[] = {0x66, 0x0F, 0x70, 0xC0, 0x00};
    /* vpbroadcastd ymm0,xmm0: VEX.L=1, proving 256-bit execution. */
    const uint8_t avx2_broadcast[] = {0xC4, 0xE2, 0x7D, 0x58, 0xC0};
    /* vpbroadcastd zmm0,xmm0: EVEX.L'L=10b, proving 512-bit execution. */
    const uint8_t avx512_broadcast[] = {0x62, 0xF2, 0x7D, 0x48, 0x58, 0xC0};

    if (!contains_sequence(sse, sse_len, sse_broadcast, sizeof(sse_broadcast)) ||
        !contains_sequence(avx2, avx2_len, avx2_broadcast, sizeof(avx2_broadcast)) ||
        !contains_sequence(avx512, avx512_len, avx512_broadcast, sizeof(avx512_broadcast))) {
        std::fprintf(stderr, "one or more SIMD modes are not using native vector width\n");
        return 1;
    }
    return 0;
}

static int assert_fail_closed_selection(void) {
    simd_width_t before = tsd_trampoline_state_current_width();
    simd_width_t target = before == SIMD_AVX2 ? SIMD_SSE41 : SIMD_AVX2;

    tsd_trampoline_force_failure(TSD_PATCH_FAIL_PROTECT_EXEC);
    if (tsd_trampoline_patch(target) == 0) {
        std::fprintf(stderr, "fault injection unexpectedly allowed transition\n");
        return 1;
    }
    if (tsd_trampoline_state_current_width() != before) {
        std::fprintf(stderr, "failed transition changed current width\n");
        return 1;
    }
    if (tsd_trampoline_inactive_page_writable() != 0) {
        std::fprintf(stderr, "failed transition exposed writable executable memory\n");
        return 1;
    }

    if (tsd_trampoline_patch(target) != 0) {
        std::fprintf(stderr, "normal immutable selection failed after injected fault\n");
        return 1;
    }
    tsd_patch_slot_t *active = tsd_trampoline_state_active();
    if (!mapping_is_rx_not_writable(active)) {
        std::fprintf(stderr, "selected target is not RX-only\n");
        return 1;
    }

    /* Legacy PKU fault injection must not alter the immutable design. */
    tsd_trampoline_force_failure(TSD_PATCH_FAIL_PKU_WINDOW);
    if (tsd_trampoline_patch(before) != 0) {
        std::fprintf(stderr, "obsolete PKU injection disturbed immutable selection\n");
        return 1;
    }
    return 0;
}

static int assert_attestation_alert(void) {
    if (tsd_trampoline_patch(SIMD_SSE41) != 0) {
        std::fprintf(stderr, "failed to refresh SSE4.1 selection\n");
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
    override_patch[10] ^= 0x01u;
    tsd_trampoline_override_patch(SIMD_SSE41, override_patch, len);
    if (tsd_trampoline_patch(SIMD_SSE41) != 0) {
        std::fprintf(stderr, "failed to install immutable override patch\n");
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
    if (assert_immutable_cet_table() != 0) {
        return 1;
    }
    if (assert_native_vector_width_payloads() != 0) {
        return 1;
    }
    if (assert_fail_closed_selection() != 0) {
        return 1;
    }
    if (assert_attestation_alert() != 0) {
        return 1;
    }
    return 0;
}
