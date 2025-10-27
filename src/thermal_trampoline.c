#include <thermal/simd/thermal_trampoline.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <thermal/simd/thermal_cpu.h>

#ifdef TSD_ENABLE_TESTS
#include "thermal_simd_test.h"
#endif

tsd_trampoline_ctx_t g_tsd_trampoline_ctx;
pthread_mutex_t g_tsd_patch_lock = PTHREAD_MUTEX_INITIALIZER;
_Atomic(simd_width_t) g_tsd_current_width;
_Atomic unsigned char g_tsd_current_width_byte;
_Atomic int g_tsd_trampoline_initialized;
_Atomic(tsd_patch_slot_t*) g_tsd_active_trampoline;
_Atomic unsigned char g_tsd_last_patch_attempt;
_Atomic unsigned char g_tsd_last_patched_width;

#ifdef TSD_ENABLE_TESTS
static const uint8_t *g_test_patch_override[3] = { NULL, NULL, NULL };
static size_t g_test_patch_override_size[3] = { 0, 0, 0 };
static char g_test_last_patch_error[256] = {0};
static int g_test_force_failure_stage = 0;
#endif

static const uint8_t k_patch_sse41[16]  = { 0xF3,0x0F,0x1E,0xFA, 0x66,0x0F,0x38,0x40,0xC1, 0xC3, 0x90,0x90,0x90,0x90,0x90,0x90 };
static const uint8_t k_patch_avx2[16]   = { 0xF3,0x0F,0x1E,0xFA, 0xC4,0xE2,0x79,0x40,0xC1, 0xC3, 0x90,0x90,0x90,0x90,0x90,0x90 };
static const uint8_t k_patch_avx512[16] = { 0xF3,0x0F,0x1E,0xFA, 0x62,0xF2,0x79,0x08,0x40,0xC1, 0xC3, 0x90,0x90,0x90,0x90,0x90 };

_Static_assert(sizeof(k_patch_sse41) == 16, "SSE41 payload must be 16 bytes");
_Static_assert(sizeof(k_patch_avx2) == 16, "AVX2 payload must be 16 bytes");
_Static_assert(sizeof(k_patch_avx512) == 16, "AVX512 payload must be 16 bytes");

static void log_errno_message(const char *prefix, int err) {
    if (!prefix) {
        return;
    }
    fprintf(stderr, "[thermal_simd] %s: %s\n", prefix, strerror(err));
#ifdef TSD_ENABLE_TESTS
    snprintf(g_test_last_patch_error, sizeof(g_test_last_patch_error), "%s: %s", prefix, strerror(err));
#endif
}

static void report_patch_error(const char *context, int err) {
    log_errno_message(context, err);
}

static void* create_rx_page(void) {
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        return NULL;
    }
    size_t pagesize = (size_t)pagesize_long;
    void *mem = mmap(NULL, pagesize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return NULL;
    }
    return mem;
}

static void* page_align(void *ptr, size_t pagesize) {
    if (!ptr) {
        return NULL;
    }
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t remainder = pagesize ? addr % pagesize : 0;
    return (void*)(addr - remainder);
}

static void serialize_instruction_stream(void) {
    __asm__ __volatile__(
        "mfence\n\t"
        "lfence\n\t"
        "cpuid\n\t"
        ::: "rax", "rbx", "rcx", "rdx", "memory"
    );
}

int tsd_trampoline_init(void) {
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        log_errno_message("failed to query page size", errno ? errno : EINVAL);
        return -1;
    }
    size_t pagesize = (size_t)pagesize_long;
    g_tsd_trampoline_ctx.page_a = create_rx_page();
    g_tsd_trampoline_ctx.page_b = create_rx_page();
    if (!g_tsd_trampoline_ctx.page_a || !g_tsd_trampoline_ctx.page_b) {
        log_errno_message("failed to allocate RX trampoline pages", errno ? errno : ENOMEM);
        return -1;
    }
    g_tsd_trampoline_ctx.active = (tsd_patch_slot_t*)g_tsd_trampoline_ctx.page_a;
    g_tsd_trampoline_ctx.inactive = (tsd_patch_slot_t*)g_tsd_trampoline_ctx.page_b;
    if (mprotect(g_tsd_trampoline_ctx.page_a, pagesize, PROT_READ | PROT_EXEC) != 0 ||
        mprotect(g_tsd_trampoline_ctx.page_b, pagesize, PROT_READ | PROT_EXEC) != 0) {
        report_patch_error("mprotect(initial RX)", errno);
        return -1;
    }
    atomic_store_explicit(&g_tsd_active_trampoline, g_tsd_trampoline_ctx.active, memory_order_seq_cst);
    atomic_store_explicit(&g_tsd_current_width, SIMD_SSE41, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_current_width_byte, (unsigned char)SIMD_SSE41, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_trampoline_initialized, 0, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_last_patch_attempt, (unsigned char)SIMD_SSE41, memory_order_relaxed);
    atomic_store_explicit(&g_tsd_last_patched_width, (unsigned char)SIMD_SSE41, memory_order_relaxed);
    return 0;
}

int init_double_buffer_trampoline(void) {
    return tsd_trampoline_init();
}

static const uint8_t* select_patch(simd_width_t width, size_t *len) {
#ifdef TSD_ENABLE_TESTS
    if (width >= SIMD_SSE41 && width <= SIMD_AVX512 && g_test_patch_override[width]) {
        if (len) {
            *len = g_test_patch_override_size[width];
        }
        return g_test_patch_override[width];
    }
#endif
    switch (width) {
        case SIMD_SSE41:
            if (len) *len = sizeof(k_patch_sse41);
            return k_patch_sse41;
        case SIMD_AVX2:
            if (len) *len = sizeof(k_patch_avx2);
            return k_patch_avx2;
        case SIMD_AVX512:
            if (len) *len = sizeof(k_patch_avx512);
            return k_patch_avx512;
        default:
            return NULL;
    }
}

void tsd_trampoline_patch(simd_width_t new_width) {
    pthread_mutex_lock(&g_tsd_patch_lock);
    simd_width_t width = atomic_load_explicit(&g_tsd_current_width, memory_order_acquire);
    int initialized = atomic_load_explicit(&g_tsd_trampoline_initialized, memory_order_acquire);
    if (initialized && new_width == width) {
        pthread_mutex_unlock(&g_tsd_patch_lock);
        return;
    }
    const uint8_t *patch_data = NULL;
    size_t patch_size = 0;
    patch_data = select_patch(new_width, &patch_size);
    if (!patch_data || patch_size == 0) {
        pthread_mutex_unlock(&g_tsd_patch_lock);
        return;
    }

    size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
    tsd_patch_slot_t *inactive = g_tsd_trampoline_ctx.inactive;
    void *inactive_page = page_align(inactive, pagesize);
    atomic_store_explicit(&g_tsd_last_patch_attempt, (unsigned char)new_width, memory_order_release);

#ifdef TSD_ENABLE_TESTS
    if (g_test_force_failure_stage == TSD_PATCH_FAIL_PROTECT_WRITE) {
        errno = EPERM;
        report_patch_error("mprotect(trampoline write)", errno);
        g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
        pthread_mutex_unlock(&g_tsd_patch_lock);
        return;
    }
#endif

    if (mprotect(inactive_page, pagesize, PROT_READ | PROT_WRITE) != 0) {
        report_patch_error("mprotect(trampoline write)", errno);
        pthread_mutex_unlock(&g_tsd_patch_lock);
        return;
    }

    for (size_t offset = 0; offset < patch_size; offset += sizeof(uint64_t)) {
        size_t chunk = sizeof(uint64_t);
        if (offset + chunk > patch_size) {
            chunk = patch_size - offset;
        }
        memcpy(inactive->code + offset, patch_data + offset, chunk);
    }

    serialize_instruction_stream();

#ifdef TSD_ENABLE_TESTS
    if (g_test_force_failure_stage == TSD_PATCH_FAIL_PROTECT_EXEC) {
        errno = EPERM;
        report_patch_error("mprotect(trampoline exec)", errno);
        g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
        pthread_mutex_unlock(&g_tsd_patch_lock);
        return;
    }
#endif

    if (mprotect(inactive_page, pagesize, PROT_READ | PROT_EXEC) != 0) {
        report_patch_error("mprotect(trampoline exec)", errno);
        pthread_mutex_unlock(&g_tsd_patch_lock);
        return;
    }

    atomic_store_explicit(&g_tsd_current_width, new_width, memory_order_release);
    atomic_store_explicit(&g_tsd_current_width_byte, (unsigned char)new_width, memory_order_release);
    atomic_store_explicit(&g_tsd_active_trampoline, inactive, memory_order_seq_cst);
    tsd_patch_slot_t *tmp = g_tsd_trampoline_ctx.active;
    g_tsd_trampoline_ctx.active = g_tsd_trampoline_ctx.inactive;
    g_tsd_trampoline_ctx.inactive = tmp;
    atomic_store_explicit(&g_tsd_trampoline_initialized, 1, memory_order_release);
    atomic_store_explicit(&g_tsd_last_patched_width, (unsigned char)new_width, memory_order_release);
    printf("Patched to %s (strict W^X)\n", new_width == SIMD_AVX512 ? "AVX-512" : new_width == SIMD_AVX2 ? "AVX2" : "SSE4.1");
    pthread_mutex_unlock(&g_tsd_patch_lock);
}

#ifdef TSD_ENABLE_TESTS
void tsd_trampoline_override_patch(simd_width_t width, const uint8_t *bytes, size_t len) {
    if (width < SIMD_SSE41 || width > SIMD_AVX512) {
        return;
    }
    g_test_patch_override[width] = bytes;
    g_test_patch_override_size[width] = len;
}

void tsd_trampoline_clear_overrides(void) {
    for (size_t i = 0; i < 3; ++i) {
        g_test_patch_override[i] = NULL;
        g_test_patch_override_size[i] = 0;
    }
}

const uint8_t* tsd_trampoline_patch_bytes(simd_width_t width, size_t *len) {
    return select_patch(width, len);
}

void tsd_trampoline_force_failure(int stage) {
    g_test_force_failure_stage = stage;
}

const char* tsd_trampoline_last_error(void) {
    return g_test_last_patch_error;
}
#endif
