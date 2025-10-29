#include <config/runtime_flags.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_int g_sandbox_complete = 0;
static atomic_int g_sandbox_success = 0;
static atomic_int g_sandbox_only = 0;
static char g_sandbox_message[256];

void tsd_runtime_flags_init(void) {
    atomic_store_explicit(&g_sandbox_complete, 0, memory_order_relaxed);
    atomic_store_explicit(&g_sandbox_success, 0, memory_order_relaxed);
    atomic_store_explicit(&g_sandbox_only, 0, memory_order_relaxed);
    (void)snprintf(g_sandbox_message, sizeof(g_sandbox_message), "%s", "sandbox pending");
}

void tsd_runtime_flags_record_sandbox_success(void) {
    atomic_store_explicit(&g_sandbox_success, 1, memory_order_release);
    atomic_store_explicit(&g_sandbox_complete, 1, memory_order_release);
    (void)snprintf(g_sandbox_message, sizeof(g_sandbox_message), "%s", "sandbox passed");
}

void tsd_runtime_flags_record_sandbox_failure(const char *message) {
    atomic_store_explicit(&g_sandbox_success, 0, memory_order_release);
    atomic_store_explicit(&g_sandbox_complete, 1, memory_order_release);
    if (message && message[0] != '\0') {
        (void)snprintf(g_sandbox_message, sizeof(g_sandbox_message), "%s", message);
    } else {
        (void)snprintf(g_sandbox_message, sizeof(g_sandbox_message), "%s", "sandbox failed");
    }
}

int tsd_runtime_flags_allow_transitions(void) {
    int complete = atomic_load_explicit(&g_sandbox_complete, memory_order_acquire);
    int success = atomic_load_explicit(&g_sandbox_success, memory_order_acquire);
    return complete && success;
}

int tsd_runtime_flags_sandbox_complete(void) {
    return atomic_load_explicit(&g_sandbox_complete, memory_order_acquire);
}

const char* tsd_runtime_flags_status_message(void) {
    return g_sandbox_message;
}

void tsd_runtime_flags_set_sandbox_only(int enabled) {
    atomic_store_explicit(&g_sandbox_only, enabled ? 1 : 0, memory_order_release);
}

int tsd_runtime_flags_sandbox_only(void) {
    return atomic_load_explicit(&g_sandbox_only, memory_order_acquire);
}
