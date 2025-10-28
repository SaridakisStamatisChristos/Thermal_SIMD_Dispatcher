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

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

extern "C" {
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
}

#if defined(__linux__)
#include <linux/mman.h>
#endif

#if defined(__linux__) && defined(__x86_64__)
#define TSD_HAVE_PKU 1
#else
#define TSD_HAVE_PKU 0
#endif

#define LOG_COMPONENT "patch"

namespace {

constexpr uint32_t kEnbr64 = 0xFA1E0FF3u; // encoded little endian ENDBR64

#if defined(TSD_ENABLE_TESTS)
extern int g_test_force_failure_stage;
#endif

static const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE> kPatchSse41 = {
    0xF3, 0x0F, 0x1E, 0xFA, 0x66, 0x0F, 0x38, 0x40, 0xC1, 0xC3,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0xF3, 0x0F, 0x1E, 0xFA, 0xC3, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

static const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE> kPatchAvx2 = {
    0xF3, 0x0F, 0x1E, 0xFA, 0xC4, 0xE2, 0x79, 0x40, 0xC1, 0xC3,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0xF3, 0x0F, 0x1E, 0xFA, 0xC3, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

static const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE> kPatchAvx512 = {
    0xF3, 0x0F, 0x1E, 0xFA, 0x62, 0xF2, 0x79, 0x08, 0x40, 0xC1,
    0xC3, 0x90, 0x90, 0x90, 0x90, 0x90,
    0xF3, 0x0F, 0x1E, 0xFA, 0xC3, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

static_assert(kPatchSse41.size() == TSD_TRAMPOLINE_SLOT_SIZE, "SSE41 patch size mismatch");
static_assert(kPatchAvx2.size() == TSD_TRAMPOLINE_SLOT_SIZE, "AVX2 patch size mismatch");
static_assert(kPatchAvx512.size() == TSD_TRAMPOLINE_SLOT_SIZE, "AVX512 patch size mismatch");

#if defined(TSD_ENABLE_TESTS)
static const uint8_t *g_test_patch_override[3] = {nullptr, nullptr, nullptr};
static size_t g_test_patch_override_size[3] = {0, 0, 0};
static char g_test_last_patch_error[256] = {0};
static std::atomic<int> g_test_last_window_mode{-1};
#endif

struct Sha256State {
    std::array<uint32_t, 8> h{};
    std::array<uint8_t, 64> buffer{};
    uint64_t total_len{0};
    size_t buffer_len{0};
};

constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

void sha256_process_block(Sha256State &state, const uint8_t *block) {
    uint32_t w[64];
    for (size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (size_t i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state.h[0];
    uint32_t b = state.h[1];
    uint32_t c = state.h[2];
    uint32_t d = state.h[3];
    uint32_t e = state.h[4];
    uint32_t f = state.h[5];
    uint32_t g = state.h[6];
    uint32_t h = state.h[7];

    for (size_t i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state.h[0] += a;
    state.h[1] += b;
    state.h[2] += c;
    state.h[3] += d;
    state.h[4] += e;
    state.h[5] += f;
    state.h[6] += g;
    state.h[7] += h;
}

void sha256_init(Sha256State &state) {
    state.h = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    state.buffer.fill(0);
    state.buffer_len = 0;
    state.total_len = 0;
}

void sha256_update(Sha256State &state, const uint8_t *data, size_t len) {
    size_t offset = 0;
    state.total_len += static_cast<uint64_t>(len);
    if (state.buffer_len > 0) {
        size_t to_copy = std::min(len, state.buffer.size() - state.buffer_len);
        std::memcpy(state.buffer.data() + state.buffer_len, data, to_copy);
        state.buffer_len += to_copy;
        offset += to_copy;
        len -= to_copy;
        if (state.buffer_len == state.buffer.size()) {
            sha256_process_block(state, state.buffer.data());
            state.buffer_len = 0;
        }
    }
    while (len >= state.buffer.size()) {
        sha256_process_block(state, data + offset);
        offset += state.buffer.size();
        len -= state.buffer.size();
    }
    if (len > 0) {
        std::memcpy(state.buffer.data(), data + offset, len);
        state.buffer_len = len;
    }
}

void sha256_finalize(Sha256State &state, uint8_t *out) {
    uint64_t bit_len = state.total_len * 8ULL;
    state.buffer[state.buffer_len++] = 0x80u;
    if (state.buffer_len > 56) {
        while (state.buffer_len < state.buffer.size()) {
            state.buffer[state.buffer_len++] = 0;
        }
        sha256_process_block(state, state.buffer.data());
        state.buffer_len = 0;
    }
    while (state.buffer_len < 56) {
        state.buffer[state.buffer_len++] = 0;
    }
    for (int i = 7; i >= 0; --i) {
        state.buffer[state.buffer_len++] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu);
    }
    sha256_process_block(state, state.buffer.data());
    for (size_t i = 0; i < state.h.size(); ++i) {
        out[i * 4] = static_cast<uint8_t>(state.h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(state.h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(state.h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(state.h[i] & 0xFFu);
    }
}

std::array<uint8_t, TSD_ATTESTATION_HASH_SIZE> sha256(const uint8_t *data, size_t len) {
    Sha256State state;
    sha256_init(state);
    sha256_update(state, data, len);
    std::array<uint8_t, TSD_ATTESTATION_HASH_SIZE> digest{};
    sha256_finalize(state, digest.data());
    return digest;
}

void log_errno_message(const char *prefix, int err) {
    if (!prefix) {
        return;
    }
    if (err == 0) {
        err = errno;
    }
    char errbuf[128];
    const char *errstr = tsd_log_strerror(err, errbuf, sizeof(errbuf));
    tsd_log_error(LOG_COMPONENT, "%s: %s", prefix, errstr);
#if defined(TSD_ENABLE_TESTS)
    std::snprintf(g_test_last_patch_error, sizeof(g_test_last_patch_error), "%s: %s", prefix, errstr);
#endif
}

void report_patch_error(const char *context, int err) {
    if (err) {
        errno = err;
    }
    log_errno_message(context, err);
}

void* create_page() {
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        return nullptr;
    }
    size_t pagesize = static_cast<size_t>(pagesize_long);
    void *mem = mmap(nullptr, pagesize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return nullptr;
    }
    return mem;
}

void* page_align(void *ptr, size_t pagesize) {
    if (!ptr || pagesize == 0) {
        return nullptr;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t remainder = addr % pagesize;
    return reinterpret_cast<void*>(addr - remainder);
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

bool has_enbr64_prefix(const uint8_t *bytes, size_t len) {
    if (!bytes || len < 4) {
        return false;
    }
    uint32_t prefix = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
    return prefix == kEnbr64;
}

const std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE>* select_patch(simd_width_t width) {
#if defined(TSD_ENABLE_TESTS)
    if (width >= SIMD_SSE41 && width <= SIMD_AVX512 && g_test_patch_override[width]) {
        static std::array<uint8_t, TSD_TRAMPOLINE_SLOT_SIZE> override{};
        size_t len = std::min(g_test_patch_override_size[width], static_cast<size_t>(TSD_TRAMPOLINE_SLOT_SIZE));
        std::memset(override.data(), 0x90, override.size());
        std::memcpy(override.data(), g_test_patch_override[width], len);
        return &override;
    }
#endif
    switch (width) {
        case SIMD_SSE41:
            return &kPatchSse41;
        case SIMD_AVX2:
            return &kPatchAvx2;
        case SIMD_AVX512:
            return &kPatchAvx512;
        default:
            return nullptr;
    }
}

#if TSD_HAVE_PKU
inline bool cpu_supports_pku() {
#if defined(__GNUC__)
    return __builtin_cpu_supports("pku");
#else
    return false;
#endif
}

inline uint32_t rdpkru() {
    uint32_t eax, edx;
    uint32_t ecx = 0;
    __asm__ __volatile__("rdpkru" : "=a"(eax), "=d"(edx) : "c"(ecx));
    return eax;
}

inline void wrpkru(uint32_t value) {
    uint32_t ecx = 0;
    uint32_t edx = 0;
    __asm__ __volatile__("wrpkru" :: "a"(value), "c"(ecx), "d"(edx) : "memory");
}
#endif

class WritableWindow {
public:
    WritableWindow(tsd_trampoline_ctx_t &ctx,
                   void *page,
                   size_t size,
                   int *prot_ptr,
                   std::atomic<bool> *effective_flag,
                   int exec_prot)
        : ctx_(ctx), page_(page), size_(size), prot_ptr_(prot_ptr),
          effective_flag_(effective_flag), exec_prot_(exec_prot),
          opened_(false), used_pku_(false), changed_prot_(false), err_(0)
#if TSD_HAVE_PKU
          , previous_pkru_(0)
#endif
    {
        if (!page_ || size_ == 0) {
            err_ = EINVAL;
            return;
        }
#if defined(TSD_ENABLE_TESTS)
        if (g_test_force_failure_stage == TSD_PATCH_FAIL_PKU_WINDOW) {
            g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
        } else
#endif
        if (ctx_.has_pku) {
#if TSD_HAVE_PKU
            used_pku_ = true;
            previous_pkru_ = rdpkru();
            uint32_t cleared = previous_pkru_ & ~ctx_.pkru_write_mask;
            wrpkru(cleared);
            opened_ = true;
            std::atomic_store_explicit(effective_flag_, true, std::memory_order_release);
#if defined(TSD_ENABLE_TESTS)
            g_test_last_window_mode.store(1, std::memory_order_release);
#endif
            return;
#endif
        }
        int target = PROT_READ | PROT_WRITE;
        if (exec_prot & PROT_EXEC) {
            target |= PROT_EXEC;
        }
        if (mprotect(page_, size_, target) != 0) {
            err_ = errno;
#if defined(TSD_ENABLE_TESTS)
            g_test_last_window_mode.store(-1, std::memory_order_release);
#endif
            return;
        }
        changed_prot_ = true;
        if (prot_ptr_) {
            *prot_ptr_ = target;
        }
        opened_ = true;
        std::atomic_store_explicit(effective_flag_, true, std::memory_order_release);
#if defined(TSD_ENABLE_TESTS)
        g_test_last_window_mode.store(0, std::memory_order_release);
#endif
    }

    ~WritableWindow() {
        if (!opened_) {
            return;
        }
        std::atomic_store_explicit(effective_flag_, false, std::memory_order_release);
        if (used_pku_) {
#if TSD_HAVE_PKU
            wrpkru(previous_pkru_);
#endif
            return;
        }
        if (changed_prot_) {
            if (mprotect(page_, size_, exec_prot_) != 0) {
                int err = errno;
                log_errno_message("mprotect(trampoline restore)", err);
            } else if (prot_ptr_) {
                *prot_ptr_ = exec_prot_;
            }
        }
    }

    bool opened() const { return opened_; }
    bool used_pku() const { return used_pku_; }
    int error() const { return err_; }

private:
    tsd_trampoline_ctx_t &ctx_;
    void *page_;
    size_t size_;
    int *prot_ptr_;
    std::atomic<bool> *effective_flag_;
    int exec_prot_;
    bool opened_;
    bool used_pku_;
    bool changed_prot_;
    int err_;
#if TSD_HAVE_PKU
    uint32_t previous_pkru_;
#endif
};

std::array<uint8_t, TSD_ATTESTATION_HASH_SIZE> g_active_hash{};
std::atomic<bool> g_active_hash_valid{false};
static char g_attestation_last_error[256] = {0};

#if defined(TSD_ENABLE_TESTS)
int g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
#endif

void update_attestation_hash(const tsd_patch_slot_t *slot) {
    if (!slot) {
        g_active_hash_valid.store(false, std::memory_order_release);
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "null slot");
        return;
    }
    g_active_hash = sha256(slot->code, TSD_TRAMPOLINE_SLOT_SIZE);
    g_active_hash_valid.store(true, std::memory_order_release);
    std::ostringstream oss;
    oss << "Trampoline hash=";
    for (uint8_t byte : g_active_hash) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(byte);
    }
    tsd_log_info(LOG_COMPONENT, "%s", oss.str().c_str());
}

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
    long pagesize_long = sysconf(_SC_PAGESIZE);
    if (pagesize_long <= 0) {
        log_errno_message("failed to query page size", errno ? errno : EINVAL);
        return -1;
    }
    size_t pagesize = static_cast<size_t>(pagesize_long);
    g_tsd_trampoline_ctx.page_a = create_page();
    g_tsd_trampoline_ctx.page_b = create_page();
    if (!g_tsd_trampoline_ctx.page_a || !g_tsd_trampoline_ctx.page_b) {
        log_errno_message("failed to allocate RX trampoline pages", errno ? errno : ENOMEM);
        return -1;
    }
    g_tsd_trampoline_ctx.page_size = pagesize;
    g_tsd_trampoline_ctx.active = reinterpret_cast<tsd_patch_slot_t*>(g_tsd_trampoline_ctx.page_a);
    g_tsd_trampoline_ctx.inactive = reinterpret_cast<tsd_patch_slot_t*>(g_tsd_trampoline_ctx.page_b);
    g_tsd_trampoline_ctx.page_a_prot = PROT_READ | PROT_WRITE;
    g_tsd_trampoline_ctx.page_b_prot = PROT_READ | PROT_WRITE;
    g_tsd_trampoline_ctx.has_pku = false;
    g_tsd_trampoline_ctx.pkey = -1;
    g_tsd_trampoline_ctx.pkru_write_mask = 0;
    g_tsd_trampoline_ctx.pkru_disable_mask = 0;
    std::atomic_store_explicit(&g_tsd_page_a_effective_writable, true, std::memory_order_relaxed);
    std::atomic_store_explicit(&g_tsd_page_b_effective_writable, true, std::memory_order_relaxed);

#if TSD_HAVE_PKU
    if (cpu_supports_pku()) {
        int key = pkey_alloc(0, 0);
        if (key >= 0) {
            int target = PROT_READ | PROT_WRITE | PROT_EXEC;
            if (pkey_mprotect(g_tsd_trampoline_ctx.page_a, pagesize, target, key) == 0 &&
                pkey_mprotect(g_tsd_trampoline_ctx.page_b, pagesize, target, key) == 0) {
                g_tsd_trampoline_ctx.has_pku = true;
                g_tsd_trampoline_ctx.pkey = key;
                unsigned mask = 0x3u << (key * 2);
                g_tsd_trampoline_ctx.pkru_disable_mask = mask;
                g_tsd_trampoline_ctx.pkru_write_mask = 0x2u << (key * 2);
                uint32_t pkru = rdpkru();
                wrpkru(pkru | g_tsd_trampoline_ctx.pkru_write_mask);
                g_tsd_trampoline_ctx.page_a_prot = PROT_READ | PROT_EXEC;
                g_tsd_trampoline_ctx.page_b_prot = PROT_READ | PROT_EXEC;
                std::atomic_store_explicit(&g_tsd_page_a_effective_writable, false, std::memory_order_release);
                std::atomic_store_explicit(&g_tsd_page_b_effective_writable, false, std::memory_order_release);
            } else {
                report_patch_error("pkey_mprotect(trampoline)", errno);
                if (key >= 0) {
                    pkey_free(key);
                }
                g_tsd_trampoline_ctx.has_pku = false;
                g_tsd_trampoline_ctx.pkey = -1;
            }
        }
    }
#endif

    if (!g_tsd_trampoline_ctx.has_pku) {
        if (mprotect(g_tsd_trampoline_ctx.page_a, pagesize, PROT_READ | PROT_EXEC) != 0 ||
            mprotect(g_tsd_trampoline_ctx.page_b, pagesize, PROT_READ | PROT_EXEC) != 0) {
            report_patch_error("mprotect(initial RX)", errno);
            return -1;
        }
        g_tsd_trampoline_ctx.page_a_prot = PROT_READ | PROT_EXEC;
        g_tsd_trampoline_ctx.page_b_prot = PROT_READ | PROT_EXEC;
        std::atomic_store_explicit(&g_tsd_page_a_effective_writable, false, std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_page_b_effective_writable, false, std::memory_order_release);
    }

    std::atomic_store_explicit(&g_tsd_active_trampoline, g_tsd_trampoline_ctx.active, std::memory_order_seq_cst);
    std::atomic_store_explicit(&g_tsd_current_width, SIMD_SSE41, std::memory_order_relaxed);
    std::atomic_store_explicit(&g_tsd_current_width_byte, static_cast<unsigned char>(SIMD_SSE41), std::memory_order_relaxed);
    std::atomic_store_explicit(&g_tsd_trampoline_initialized, 0, std::memory_order_relaxed);
    std::atomic_store_explicit(&g_tsd_last_patch_attempt, static_cast<unsigned char>(SIMD_SSE41), std::memory_order_relaxed);
    std::atomic_store_explicit(&g_tsd_last_patched_width, static_cast<unsigned char>(SIMD_SSE41), std::memory_order_relaxed);
    g_active_hash_valid.store(false, std::memory_order_release);

#if defined(TSD_ENABLE_TESTS)
    g_test_last_window_mode.store(-1, std::memory_order_relaxed);
#endif
    return 0;
}

int init_double_buffer_trampoline(void) {
    return tsd_trampoline_init();
}

int tsd_trampoline_patch(simd_width_t new_width) {
    pthread_mutex_lock(&g_tsd_patch_lock);
    simd_width_t width = std::atomic_load_explicit(&g_tsd_current_width, std::memory_order_acquire);
    simd_width_t previous_width = width;
    int initialized = std::atomic_load_explicit(&g_tsd_trampoline_initialized, std::memory_order_acquire);
    int rc = -1;

    do {
        if (initialized && new_width == width) {
            rc = 0;
            break;
        }

        const auto *patch = select_patch(new_width);
        if (!patch) {
            report_patch_error("invalid SIMD width", EINVAL);
            break;
        }
        if (!has_enbr64_prefix(patch->data(), patch->size())) {
            report_patch_error("missing ENDBR64 landing pad", EINVAL);
            break;
        }

        long pagesize_long = sysconf(_SC_PAGESIZE);
        if (pagesize_long <= 0) {
            report_patch_error("sysconf(_SC_PAGESIZE)", errno ? errno : EINVAL);
            break;
        }
        size_t pagesize = static_cast<size_t>(pagesize_long);
        if (g_tsd_trampoline_ctx.page_size == 0) {
            g_tsd_trampoline_ctx.page_size = pagesize;
        }

        tsd_patch_slot_t *inactive = g_tsd_trampoline_ctx.inactive;
        void *inactive_page = page_align(inactive, pagesize);
        int *inactive_prot_ptr = (inactive_page == g_tsd_trampoline_ctx.page_a)
            ? &g_tsd_trampoline_ctx.page_a_prot
            : (inactive_page == g_tsd_trampoline_ctx.page_b ? &g_tsd_trampoline_ctx.page_b_prot : nullptr);
        std::atomic<bool> *inactive_effective = (inactive_page == g_tsd_trampoline_ctx.page_a)
            ? &g_tsd_page_a_effective_writable
            : &g_tsd_page_b_effective_writable;
        int original_prot = inactive_prot_ptr ? *inactive_prot_ptr : PROT_NONE;
        std::atomic_store_explicit(&g_tsd_last_patch_attempt, static_cast<unsigned char>(new_width), std::memory_order_release);

#if defined(TSD_ENABLE_TESTS)
        if (g_test_force_failure_stage == TSD_PATCH_FAIL_PROTECT_WRITE) {
            report_patch_error("mprotect(trampoline write)", EPERM);
            g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
            break;
        }
#endif

        bool window_opened = false;
        {
            WritableWindow window(g_tsd_trampoline_ctx, inactive_page, pagesize, inactive_prot_ptr,
                                  inactive_effective, original_prot == PROT_NONE ? (PROT_READ | PROT_EXEC) : original_prot);
            if (!window.opened()) {
                report_patch_error("trampoline write window", window.error());
                break;
            }
            window_opened = true;

            std::memcpy(inactive->code, patch->data(), TSD_TRAMPOLINE_SLOT_SIZE);
            serialize_instruction_stream();

#if defined(TSD_ENABLE_TESTS)
            if (g_test_force_failure_stage == TSD_PATCH_FAIL_PROTECT_EXEC) {
                report_patch_error("mprotect(trampoline exec)", EPERM);
                g_test_force_failure_stage = TSD_PATCH_FAIL_NONE;
                break;
            }
#endif
        }

        if (!window_opened) {
            break;
        }

        std::atomic_store_explicit(&g_tsd_current_width, new_width, std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_current_width_byte, static_cast<unsigned char>(new_width), std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_active_trampoline, inactive, std::memory_order_seq_cst);
        tsd_patch_slot_t *tmp = g_tsd_trampoline_ctx.active;
        g_tsd_trampoline_ctx.active = g_tsd_trampoline_ctx.inactive;
        g_tsd_trampoline_ctx.inactive = tmp;
        std::atomic_store_explicit(&g_tsd_trampoline_initialized, 1, std::memory_order_release);
        std::atomic_store_explicit(&g_tsd_last_patched_width, static_cast<unsigned char>(new_width), std::memory_order_release);

        tsd_log_info(LOG_COMPONENT, "Patched to %s (CET-aligned)",
                     new_width == SIMD_AVX512 ? "AVX-512" :
                     new_width == SIMD_AVX2 ? "AVX2" : "SSE4.1");
        update_attestation_hash(std::atomic_load_explicit(&g_tsd_active_trampoline, std::memory_order_acquire));
        rc = 0;
    } while (0);

    tsd_metrics_record_width_transition(previous_width, new_width, rc);
    pthread_mutex_unlock(&g_tsd_patch_lock);
    return rc;
}

int tsd_trampoline_self_validate(char *reason, size_t reason_len) {
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
    const auto *expected = select_patch(width);
    if (!expected) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "unknown width %d", static_cast<int>(width));
        }
        return -1;
    }
    tsd_patch_slot_t *active = std::atomic_load_explicit(&g_tsd_active_trampoline, std::memory_order_acquire);
    if (!active) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "active slot null");
        }
        return -1;
    }
    if (std::memcmp(active->code, expected->data(), TSD_TRAMPOLINE_SLOT_SIZE) != 0) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "active slot checksum mismatch");
        }
        return -1;
    }
    bool page_a_writable = std::atomic_load_explicit(&g_tsd_page_a_effective_writable, std::memory_order_acquire);
    bool page_b_writable = std::atomic_load_explicit(&g_tsd_page_b_effective_writable, std::memory_order_acquire);
    if (!g_tsd_trampoline_ctx.has_pku &&
        ((g_tsd_trampoline_ctx.page_a_prot & PROT_WRITE) || (g_tsd_trampoline_ctx.page_b_prot & PROT_WRITE))) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "trampoline page writable");
        }
        return -1;
    }
    if (page_a_writable || page_b_writable) {
        if (reason && reason_len > 0) {
            std::snprintf(reason, reason_len, "%s", "write window open");
        }
        return -1;
    }
    return 0;
}

#if defined(TSD_ENABLE_TESTS)
void tsd_trampoline_override_patch(simd_width_t width, const uint8_t *bytes, size_t len) {
    if (width < SIMD_SSE41 || width > SIMD_AVX512) {
        return;
    }
    g_test_patch_override[width] = bytes;
    g_test_patch_override_size[width] = len;
}

void tsd_trampoline_clear_overrides(void) {
    for (size_t i = 0; i < 3; ++i) {
        g_test_patch_override[i] = nullptr;
        g_test_patch_override_size[i] = 0;
    }
}

const uint8_t* tsd_trampoline_patch_bytes(simd_width_t width, size_t *len) {
    const auto *patch = select_patch(width);
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
    size_t pagesize = g_tsd_trampoline_ctx.page_size;
    if (pagesize == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        if (ps <= 0) {
            return -1;
        }
        pagesize = static_cast<size_t>(ps);
    }
    if (!g_tsd_trampoline_ctx.inactive) {
        return -1;
    }
    void *inactive_page = page_align(g_tsd_trampoline_ctx.inactive, pagesize);
    std::atomic<bool> *flag = (inactive_page == g_tsd_trampoline_ctx.page_a)
        ? &g_tsd_page_a_effective_writable
        : &g_tsd_page_b_effective_writable;
    return std::atomic_load_explicit(flag, std::memory_order_acquire) ? 1 : 0;
}

int tsd_trampoline_test_last_window_used_pku(void) {
    return g_test_last_window_mode.load(std::memory_order_acquire);
}
#endif

} // extern "C"

extern "C" {

int tsd_attestation_get_active_hash(uint8_t *buffer, size_t len) {
    if (!buffer || len < TSD_ATTESTATION_HASH_SIZE) {
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "buffer too small");
        return -1;
    }
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
    if (!g_active_hash_valid.load(std::memory_order_acquire)) {
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", "hash unavailable");
        return -1;
    }
    if (std::memcmp(expected, g_active_hash.data(), TSD_ATTESTATION_HASH_SIZE) != 0) {
        std::ostringstream oss;
        oss << "attestation mismatch expected=";
        for (size_t i = 0; i < TSD_ATTESTATION_HASH_SIZE; ++i) {
            oss << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(expected[i]);
        }
        oss << " actual=";
        for (uint8_t byte : g_active_hash) {
            oss << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(byte);
        }
        std::string msg = oss.str();
        std::snprintf(g_attestation_last_error, sizeof(g_attestation_last_error), "%s", msg.c_str());
        tsd_log_error(LOG_COMPONENT, "%s", msg.c_str());
        return -1;
    }
    g_attestation_last_error[0] = '\0';
    return 0;
}

const char* tsd_attestation_last_error(void) {
    return g_attestation_last_error;
}

} // extern "C"
