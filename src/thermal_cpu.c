#include <thermal/simd/thermal_cpu.h>

#include <cpuid.h>
#include <stddef.h>

#ifdef TSD_ENABLE_TESTS
static simd_width_t (*g_detect_override)(void) = NULL;
#endif

/*
 * Kept as a byte because the naked assembly shim addresses this symbol
 * directly. C-side accesses use GCC/Clang atomic builtins so concurrent host
 * capability probes cannot introduce a language-level write/write race.
 */
uint8_t g_tsd_avx_available = 0;

int tsd_cpu_avx_available(void) {
    return __atomic_load_n(&g_tsd_avx_available, __ATOMIC_ACQUIRE) != 0;
}

int tsd_cpu_has_sse41(void) {
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return 0;
    }
    return (ecx & (1u << 19)) != 0;
}

static int cpu_has_avx_bit(void) {
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return 0;
    }
    return (ecx & (1u << 28)) != 0;
}

static int check_xsave_support(void) {
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return 0;
    }
    if (!(ecx & (1u << 27))) {
        return 0;
    }
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    uint64_t xcr0 = ((uint64_t)edx << 32) | eax;
    return (xcr0 & 0x6) == 0x6;
}

static simd_width_t detect_impl(const tsd_runtime_config *cfg) {
    if (!tsd_cpu_has_sse41()) {
        return SIMD_SSE41;
    }
    int xsave_ok = check_xsave_support();
    if (!xsave_ok) {
        return SIMD_SSE41;
    }
    if (cpu_has_avx_bit()) {
        __atomic_store_n(&g_tsd_avx_available, 1, __ATOMIC_RELEASE);
    }
    unsigned int eax, ebx, ecx, edx;
    if (cfg && cfg->allow_avx512 && __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if ((ebx & (1 << 16)) && cpu_has_avx_bit()) {
            __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
            uint64_t xcr0 = ((uint64_t)edx << 32) | eax;
            if ((xcr0 & 0xE6) == 0xE6) {
                return SIMD_AVX512;
            }
        }
        if ((ebx & (1 << 5)) && cpu_has_avx_bit()) {
            return SIMD_AVX2;
        }
    } else if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if ((ebx & (1 << 5)) && cpu_has_avx_bit()) {
            return SIMD_AVX2;
        }
    }
    return SIMD_SSE41;
}

simd_width_t tsd_detect_max_simd(const tsd_runtime_config *cfg) {
#ifdef TSD_ENABLE_TESTS
    if (g_detect_override) {
        return g_detect_override();
    }
#endif
    return detect_impl(cfg);
}

#ifdef TSD_ENABLE_TESTS
void tsd_cpu_set_detect_override(simd_width_t (*fn)(void)) {
    g_detect_override = fn;
}

void tsd_cpu_clear_detect_override(void) {
    g_detect_override = NULL;
}

simd_width_t tsd_cpu_detect_ignoring_override(const tsd_runtime_config *cfg) {
    return detect_impl(cfg);
}
#endif
