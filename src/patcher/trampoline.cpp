#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <patcher/attestation.h>
#include <thermal/simd/thermal_trampoline.h>

#ifdef TSD_ENABLE_TESTS
#define TSD_INTERNAL_ATOMIC_WRAP(T) std::atomic<T>
#define _Atomic(T) TSD_INTERNAL_ATOMIC_WRAP(T)
extern "C" {
#include "thermal_simd_test.h"
}
#undef _Atomic
#undef TSD_INTERNAL_ATOMIC_WRAP
#endif

#include <thermal/simd/logging.h>
#include <thermal/simd/metrics.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_cpu.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

#include <openssl/sha.h>

extern "C" {
#include <sys/mman.h>
#include <unistd.h>
}

#define LOG_COMPONENT "patch"

namespace {

constexpr uint32_t kEnbr64 = 0xFA1E0FF3u;
constexpr size_t kWidthCount = 3;
constexpr size_t kSlotStride = alignof(tsd_patch_slot_t);

static const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE> kPatchSse41 = {
    0xF3, 0x0F, 0x1E, 0xFA,
    0x66, 0x0F, 0x70, 0xC0, 0x00,
    0x66, 0x0F, 0x70, 0xC9, 0x00,
    0x66, 0x0F, 0x38, 0x40, 0xC1,
    0xC3,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

static const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE> kPatchAvx2 = {
    0xF3, 0x0F, 0x1E, 0xFA,
    0xC4, 0xE2, 0x7D, 0x58, 0xC0,
    0xC4, 0xE2, 0x7D, 0x58, 0xC9,
    0xC4, 0xE2, 0x7D, 0x40, 0xC1,
    0xC5, 0xF8, 0x77,
    0xC3,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

static const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE> kPatchAvx512 = {
    0xF3, 0x0F, 0x1E, 0xFA,
    0x62, 0xF2, 0x7D, 0x48, 0x58, 0xC0,
    0x62, 0xF2, 0x7D, 0x48, 0x58, 0xC9,
    0x62, 0xF2, 0x7D, 0x48, 0x40, 0xC1,
    0xC5, 0xF8, 0x77,
    0xC3,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

static_assert(kPatchSse41.size() == TSD_TRAMPOLINE_SLOT_SIZE, "SSE41 patch size mismatch");
static_assert(kPatchAvx2.size() == TSD_TRAMPOLINE_SLOT_SIZE, "AVX2 patch size mismatch");
static_assert(kPatchAvx512.size() == TSD_TRAMPOLINE_SLOT_SIZE, "AVX512 patch size mismatch");
static_assert(kSlotStride >= TSD_TRAMPOLINE_SLOT_SIZE, "slot alignment must cover code payload");

std::array<tsd_patch_slot_t*, kWidthCount> g_slots{nullptr, nullptr, nullptr};
std::array<uint8_t, TSD_ATTESTATION_HASH_SIZE> g_active_hash{};
std::atomic<bool> g_active_hash_valid{false};
std::mutex g_attestation_mutex;
thread_local char g_attestation_last_error[256] = {0};

#ifdef TSD_ENABLE_TESTS
const uint8_t *g_test_patch_override[kWidthCount] = {nullptr, nullptr, nullptr};
size_t g_test_patch_override_size[kWidthCount] = {0, 0, 0};
char g_test_last_patch_error[256] = {0};
std::vector<void*> g_test_override_pages;
int g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
#endif

class PthreadLockGuard {
public:
    explicit PthreadLockGuard(pthread_mutex_t *mutex) : mutex_(mutex) {
        pthread_mutex_lock(mutex_);
    }
    ~PthreadLockGuard() { pthread_mutex_unlock(mutex_); }
    PthreadLockGuard(const PthreadLockGuard &) = delete;
    PthreadLockGuard &operator=(const PthreadLockGuard &) = delete;
private:
    pthread_mutex_t *mutex_;
};

const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE>* canonical_patch(simd_width_t width) {
    switch (width) {
        case SIMD_SSE41: return &kPatchSse41;
        case SIMD_AVX2: return &kPatchAvx2;
        case SIMD_AVX512: return &kPatchAvx512;
        default: return nullptr;
    }
}

const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE>* selected_patch(simd_width_t width) {
#ifdef TSD_ENABLE_TESTS
    if (width >= SIMD_SSE41 && width <= SIMD_AVX512 && g_test_patch_override[width]) {
        static std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE> override{};
        override.fill(0x90);
        size_t len = g_test_patch_override_size[width];
        if (len > override.size()) {
            len = override.size();
        }
        std::memcpy(override.data(), g_test_patch_override[width], len);
        return &override;
    }
#endif
    return canonical_patch(width);
}

bool has_enbr64_prefix(const uint8_t *bytes, size_t len) {
    if (!bytes || len < sizeof(uint32_t)) {
        return false;
    }
    uint32_t prefix = static_cast<uint32_t>(bytes[0]) |
                      (static_cast<uint32_t>(bytes[1]) << 8) |
                      (static_cast<uint32_t>(bytes[2]) << 16) |
                      (static_cast<uint32_t>(bytes[3]) << 24);
    return prefix == kEnbr64;
}

void serialize_instruction_stream() {
    __asm__ __volatile__(
        "mfence\n\t"
        "lfence\n\t"
        "cpuid\n\t"
        :
        :
        : "rax", "rbx", "rcx", "rdx", "memory");
}

void set_error(const char *message, int err) {
    if (!message) {
        message = "patch error";
    }
    if (err != 0) {
        errno = err;
    }
    char errbuf[128] = {0};
    const char *detail = err ? tsd_log_strerror(err, errbuf, sizeof(errbuf)) : nullptr;
    if (detail) {
        tsd_log_error(LOG_COMPONENT, "%s: %s", message, detail);
#ifdef TSD_ENABLE_TESTS
        std::snprintf(g_test_last_patch_error, sizeof(g_test_last_patch_error), "%s: %s", message, detail);
#endif
    } else {
        tsd_log_error(LOG_COMPONENT, "%s", message);
#ifdef TSD_ENABLE_TESTS
        std::snprintf(g_test_last_patch_error, sizeof(g_test_last_patch_error), "%s", message);
#endif
    }
}

bool mapping_is_rx_not_writable(const void *address) {
    if (!address) {
        return false;
    }
    FILE *maps = std::fopen("/proc/self/maps", "r");
    if (!maps) {
        return false;
    }
    uintptr_t needle = reinterpret_cast<uintptr_t>(address);
    char line[512];
    bool ok = false;
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

void discard_code_page(void *page, size_t pagesize) {
    if (page && page != MAP_FAILED && pagesize > 0) {
        munmap(page, pagesize);
    }
    g_tsd_trampoline_ctx.page_a = nullptr;
    g_tsd_trampoline_ctx.page_b = nullptr;
    g_tsd_trampoline_ctx.page_size = 0;
    g_tsd_trampoline_ctx.page_a_prot = PROT_NONE;
    g_tsd_trampoline_ctx.page_b_prot = PROT_NONE;
    g_tsd_trampoline_ctx.active = nullptr;
    g_tsd_trampoline_ctx.inactive = nullptr;
    for (auto &slot : g_slots) {
        slot = nullptr;
    }
    std::atomic_store_explicit(&g_tsd_page_a_effective_writable, false, std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_page_b_effective_writable, false, std::memory_order_release);
}

void update_attestation_hash(const tsd_patch_slot_t *slot) {
    std::lock_guard<std::mutex> lock(g_attestation_mutex);
    g_active_hash_valid.store(false, std::memory_order_release);
    if (!slot) {
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "null slot");
        return;
    }
    SHA256(slot->code, TSD_TRAMPOLINE_SLOT_SIZE, g_active_hash.data());
    g_active_hash_valid.store(true, std::memory_order_release);

    std::ostringstream oss;
    oss << "Trampoline hash=";
    for (uint8_t byte : g_active_hash) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
    }
    tsd_log_info(LOG_COMPONENT, "%s", oss.str().c_str());
}

int build_canonical_code_page() {
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        set_error("failed to query page size", errno ? errno : EINVAL);
        return -1;
    }
    size_t pagesize = static_cast<size_t>(pagesize_long);
    if (kWidthCount * sizeof(tsd_patch_slot_t) > pagesize) {
        set_error("trampoline table exceeds page size", E2BIG);
        return -1;
    }

    void *page = mmap(nullptr, pagesize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        set_error("mmap(trampoline table)", errno);
        return -1;
    }

    g_tsd_trampoline_ctx.page_a = page;
    g_tsd_trampoline_ctx.page_b = nullptr;
    g_tsd_trampoline_ctx.page_size = pagesize;
    g_tsd_trampoline_ctx.page_a_prot = PROT_READ | PROT_WRITE;
    g_tsd_trampoline_ctx.page_b_prot = PROT_NONE;
    g_tsd_trampoline_ctx.has_pku = false;
    g_tsd_trampoline_ctx.pkey = -1;
    g_tsd_trampoline_ctx.pkru_write_mask = 0;
    g_tsd_trampoline_ctx.pkru_disable_mask = 0;
    std::atomic_store_explicit(&g_tsd_page_a_effective_writable, true, std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_page_b_effective_writable, false, std::memory_order_release);

    auto *slots = reinterpret_cast<tsd_patch_slot_t*>(page);
    for (size_t i = 0; i < kWidthCount; ++i) {
        simd_width_t width = static_cast<simd_width_t>(i);
        const auto *patch = canonical_patch(width);
        if (!patch || !has_enbr64_prefix(patch->data(), patch->size())) {
            discard_code_page(page, pagesize);
            set_error("invalid canonical trampoline payload", EINVAL);
            return -1;
        }
        g_slots[i] = &slots[i];
        std::memcpy(g_slots[i]->code, patch->data(), TSD_TRAMPOLINE_SLOT_SIZE);
    }

    serialize_instruction_stream();
    if (mprotect(page, pagesize, PROT_READ | PROT_EXEC) != 0) {
        int err = errno;
        discard_code_page(page, pagesize);
        set_error("mprotect(trampoline table RX)", err);
        return -1;
    }

    g_tsd_trampoline_ctx.page_a_prot = PROT_READ | PROT_EXEC;
    std::atomic_store_explicit(&g_tsd_page_a_effective_writable, false, std::memory_order_release);

    if (!mapping_is_rx_not_writable(page)) {
        discard_code_page(page, pagesize);
        set_error("trampoline table failed RX verification", EPERM);
        return -1;
    }
    return 0;
}

#ifdef TSD_ENABLE_TESTS
tsd_patch_slot_t* build_test_override_slot(const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE> &patch) {
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        return nullptr;
    }
    size_t pagesize = static_cast<size_t>(pagesize_long);
    void *page = mmap(nullptr, pagesize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        return nullptr;
    }
    auto *slot = reinterpret_cast<tsd_patch_slot_t*>(page);
    std::memcpy(slot->code, patch.data(), TSD_TRAMPOLINE_SLOT_SIZE);
    serialize_instruction_stream();
    if (mprotect(page, pagesize, PROT_READ | PROT_EXEC) != 0) {
        munmap(page, pagesize);
        return nullptr;
    }
    if (!mapping_is_rx_not_writable(page)) {
        munmap(page, pagesize);
        return nullptr;
    }
    g_test_override_pages.push_back(page);
    return slot;
}
#endif

} // namespace

extern "C" {

tsd_trampoline_ctx_t g_tsd_trampoline_ctx = {0};
pthread_mutex_t g_tsd_patch_lock = PTHREAD_MUTEX_INITIALIZER;
std::atomic<simd_width_t> g_tsd_current_width{SIMD_SSE41};
std::atomic<unsigned char> g_tsd_current_width_byte{static_cast<unsigned char>(SIMD_SSE41)};
std::atomic<int> g_tsd_trampoline_initialized{0};
std::atomic<tsd_patch_slot_t*> g_tsd_active_trampoline{nullptr};
std::atomic<unsigned char> g_tsd_last_patch_attempt{static_cast<unsigned char>(SIMD_SSE41)};
std::atomic<unsigned char> g_tsd_last_patched_width{static_cast<unsigned char>(SIMD_SSE41)};
std::atomic<bool> g_tsd_page_a_effective_writable{false};
std::atomic<bool> g_tsd_page_b_effective_writable{false};

int tsd_trampoline_init(void) {
    pthread_mutex_lock(&g_tsd_patch_lock);

    if (!g_tsd_trampoline_ctx.page_a) {
        if (build_canonical_code_page() != 0) {
            pthread_mutex_unlock(&g_tsd_patch_lock);
            return -1;
        }
    }

    g_tsd_trampoline_ctx.active = g_slots[SIMD_SSE41];
    g_tsd_trampoline_ctx.inactive = g_slots[SIMD_AVX2];
    std::atomic_store_explicit(&g_tsd_active_trampoline, g_slots[SIMD_SSE41], std::memory_order_seq_cst);
    std::atomic_store_explicit(&g_tsd_current_width, SIMD_SSE41, std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_current_width_byte, static_cast<unsigned char>(SIMD_SSE41), std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_trampoline_initialized, 0, std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_last_patch_attempt, static_cast<unsigned char>(SIMD_SSE41), std::memory_order_release);
    std::atomic_store_explicit(&g_tsd_last_patched_width, static_cast<unsigned char>(SIMD_SSE41), std::memory_order_release);
    {
        std::lock_guard<std::mutex> hash_lock(g_attestation_mutex);
        g_active_hash_valid.store(false, std::memory_order_release);
    }

    pthread_mutex_unlock(&g_tsd_patch_lock);
    return 0;
}

int init_double_buffer_trampoline(void) {
    return tsd_trampoline_init();
}

int tsd_trampoline_patch(simd_width_t new_width) {
    pthread_mutex_lock(&g_tsd_patch_lock);

    simd_width_t previous_width = std::atomic_load_explicit(&g_tsd_current_width, std::memory_order_acquire);
    std::atomic_store_explicit(&g_tsd_last_patch_attempt, static_cast<unsigned char>(new_width), std::memory_order_release);
    int rc = -1;

    do {
        if (new_width < SIMD_SSE41 || new_width > SIMD_AVX512) {
            set_error("invalid SIMD width", EINVAL);
            break;
        }
#ifndef TSD_ENABLE_TESTS
        if (!tsd_cpu_has_sse41()) {
            set_error("SSE4.1 is not supported by this host", ENOTSUP);
            break;
        }
        simd_width_t allowed_width = tsd_detect_max_simd(&g_tsd_config);
        if (new_width > allowed_width) {
            set_error("requested SIMD width exceeds host or runtime policy", ENOTSUP);
            break;
        }
#endif
        if (!g_tsd_trampoline_ctx.page_a && build_canonical_code_page() != 0) {
            break;
        }

#ifdef TSD_ENABLE_TESTS
        if (g_test_force_failure_stage == TSD_PATCH_FAIL_PROTECT_WRITE) {
            g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
            set_error("immutable trampoline selection rejected (write fault injection)", EPERM);
            break;
        }
        if (g_test_force_failure_stage == TSD_PATCH_FAIL_PROTECT_EXEC) {
            g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
            set_error("immutable trampoline selection rejected (RX fault injection)", EPERM);
            break;
        }
        if (g_test_force_failure_stage == TSD_PATCH_FAIL_PKU_WINDOW) {
            g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
        }
#endif

        const auto *patch = selected_patch(new_width);
        if (!patch || !has_enbr64_prefix(patch->data(), patch->size())) {
            set_error("missing ENDBR64 landing pad", EINVAL);
            break;
        }

        tsd_patch_slot_t *target = g_slots[static_cast<size_t>(new_width)];
#ifdef TSD_ENABLE_TESTS
        if (g_test_patch_override[new_width]) {
            target = build_test_override_slot(*patch);
            if (!target) {
                set_error("failed to build immutable test override", errno ? errno : EPERM);
                break;
            }
        }
#endif
        if (!target || !mapping_is_rx_not_writable(target)) {
            set_error("target trampoline is not immutable RX", EPERM);
            break;
        }

        tsd_patch_slot_t *old_active = std::atomic_load_explicit(&g_tsd_active_trampoline, std::memory_order_acquire);
        g_tsd_trampoline_ctx.active = target;
        g_tsd_trampoline_ctx.inactive = old_active;

        std::atomic_store_explicit(&g_tsd_current_width, new_width, std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_current_width_byte, static_cast<unsigned char>(new_width), std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_active_trampoline, target, std::memory_order_seq_cst);
        std::atomic_store_explicit(&g_tsd_trampoline_initialized, 1, std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_last_patched_width, static_cast<unsigned char>(new_width), std::memory_order_release);

        update_attestation_hash(target);
        tsd_log_info(LOG_COMPONENT, "Selected immutable %s trampoline",
                     new_width == SIMD_AVX512 ? "AVX-512/512-bit" :
                     new_width == SIMD_AVX2 ? "AVX2/256-bit" : "SSE4.1/128-bit");
        rc = 0;
    } while (0);

    tsd_metrics_record_width_transition(previous_width, new_width, rc);
    pthread_mutex_unlock(&g_tsd_patch_lock);
    return rc;
}

int tsd_trampoline_self_validate(char *reason, size_t reason_len) {
    PthreadLockGuard patch_guard(&g_tsd_patch_lock);
    if (reason && reason_len > 0) {
        reason[0] = '\0';
    }
    if (!std::atomic_load_explicit(&g_tsd_trampoline_initialized, std::memory_order_acquire)) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "trampoline not initialised");
        }
        return -1;
    }

    simd_width_t width = std::atomic_load_explicit(&g_tsd_current_width, std::memory_order_acquire);
    const auto *expected = selected_patch(width);
    tsd_patch_slot_t *active = std::atomic_load_explicit(&g_tsd_active_trampoline, std::memory_order_acquire);
    if (!expected || !active) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "active trampoline unavailable");
        }
        return -1;
    }
    if (std::memcmp(active->code, expected->data(), TSD_TRAMPOLINE_SLOT_SIZE) != 0) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "active slot checksum mismatch");
        }
        return -1;
    }
    if (!has_enbr64_prefix(active->code, TSD_TRAMPOLINE_SLOT_SIZE)) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "active slot missing ENDBR64");
        }
        return -1;
    }
    if (!mapping_is_rx_not_writable(active)) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "active trampoline is not RX-only");
        }
        return -1;
    }
    if (std::atomic_load_explicit(&g_tsd_page_a_effective_writable, std::memory_order_acquire) ||
        std::atomic_load_explicit(&g_tsd_page_b_effective_writable, std::memory_order_acquire)) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "trampoline page writable");
        }
        return -1;
    }
    return 0;
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
    for (size_t i = 0; i < kWidthCount; ++i) {
        g_test_patch_override[i] = nullptr;
        g_test_patch_override_size[i] = 0;
    }
}

const uint8_t* tsd_trampoline_patch_bytes(simd_width_t width, size_t *len) {
    const auto *patch = selected_patch(width);
    if (!patch) {
        if (len) {
            *len = 0;
        }
        return nullptr;
    }
    if (len) {
        *len = patch->size();
    }
    return patch->data();
}

void tsd_trampoline_force_failure(int stage) {
    g_test_force_failure_stage = stage;
}

const char* tsd_trampoline_last_error(void) {
    return g_test_last_patch_error;
}

int tsd_trampoline_inactive_page_writable(void) {
    return 0;
}

int tsd_trampoline_test_last_window_used_pku(void) {
    return 0;
}
#endif

int tsd_attestation_get_active_hash(uint8_t *buffer, size_t len) {
    if (!buffer || len < TSD_ATTESTATION_HASH_SIZE) {
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "buffer too small");
        return -1;
    }
    PthreadLockGuard patch_guard(&g_tsd_patch_lock);
    std::lock_guard<std::mutex> hash_lock(g_attestation_mutex);
    if (!g_active_hash_valid.load(std::memory_order_acquire)) {
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "hash unavailable");
        return -1;
    }
    std::memcpy(buffer, g_active_hash.data(), TSD_ATTESTATION_HASH_SIZE);
    return 0;
}

int tsd_attestation_get_active_hash_hex(char *buffer, size_t len) {
    if (!buffer || len < (TSD_ATTESTATION_HASH_SIZE * 2 + 1)) {
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "hex buffer too small");
        return -1;
    }
    PthreadLockGuard patch_guard(&g_tsd_patch_lock);
    std::lock_guard<std::mutex> hash_lock(g_attestation_mutex);
    if (!g_active_hash_valid.load(std::memory_order_acquire)) {
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "hash unavailable");
        return -1;
    }
    for (size_t i = 0; i < g_active_hash.size(); ++i) {
        std::snprintf(buffer + (i * 2), len - (i * 2), "%02x", g_active_hash[i]);
    }
    buffer[TSD_ATTESTATION_HASH_SIZE * 2] = '\0';
    return 0;
}

int tsd_attestation_expect_active_hash(const uint8_t *expected, size_t len) {
    if (!expected || len < TSD_ATTESTATION_HASH_SIZE) {
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "expected hash invalid");
        return -1;
    }
    PthreadLockGuard patch_guard(&g_tsd_patch_lock);
    std::lock_guard<std::mutex> hash_lock(g_attestation_mutex);
    if (!g_active_hash_valid.load(std::memory_order_acquire)) {
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "hash unavailable");
        return -1;
    }
    if (std::memcmp(expected, g_active_hash.data(), TSD_ATTESTATION_HASH_SIZE) != 0) {
        std::ostringstream oss;
        oss << "attestation mismatch expected=";
        for (size_t i = 0; i < TSD_ATTESTATION_HASH_SIZE; ++i) {
            oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(expected[i]);
        }
        oss << " actual=";
        for (uint8_t byte : g_active_hash) {
            oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
        }
        std::string message = oss.str();
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", message.c_str());
        tsd_log_error(LOG_COMPONENT, "%s", message.c_str());
        return -1;
    }
    g_attestation_last_error[0] = '\0';
    return 0;
}

const char* tsd_attestation_last_error(void) {
    return g_attestation_last_error;
}

} // extern "C"
