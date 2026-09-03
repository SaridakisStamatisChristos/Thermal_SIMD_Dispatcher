#include <config/runtime_flags.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <thermal/simd/telemetry_fusion.h>

#include "../runtime_guard_internal.h"

static atomic_int g_sandbox_complete = 0;
static atomic_int g_sandbox_success = 0;
static atomic_int g_sandbox_only = 0;
static pthread_mutex_t g_sandbox_message_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_sandbox_message[256] = "sandbox pending";
static _Thread_local char g_sandbox_message_snapshot[256];

static void set_message_locked(const char *message) {
    const char *text = (message && message[0] != '\0') ? message : "sandbox failed";
    (void)snprintf(g_sandbox_message, sizeof(g_sandbox_message), "%s", text);
}

void tsd_runtime_flags_init(void) {
    if (tsd_runtime_safety_write_enter() != 0) return;
    pthread_mutex_lock(&g_sandbox_message_lock);
    set_message_locked("sandbox pending");
    atomic_store_explicit(&g_sandbox_success, 0, memory_order_relaxed);
    atomic_store_explicit(&g_sandbox_complete, 0, memory_order_release);
    pthread_mutex_unlock(&g_sandbox_message_lock);
    tsd_runtime_safety_write_leave();
    atomic_store_explicit(&g_sandbox_only, 0, memory_order_release);
}

void tsd_runtime_flags_record_sandbox_success(void) {
    if (tsd_runtime_safety_write_enter() != 0) return;
    pthread_mutex_lock(&g_sandbox_message_lock);
    set_message_locked("sandbox passed");
    /* Publish the explanatory payload before making the completed state visible. */
    atomic_store_explicit(&g_sandbox_success, 1, memory_order_relaxed);
    atomic_store_explicit(&g_sandbox_complete, 1, memory_order_release);
    pthread_mutex_unlock(&g_sandbox_message_lock);
    tsd_runtime_safety_write_leave();
}

void tsd_runtime_flags_record_sandbox_failure(const char *message) {
    if (tsd_runtime_safety_write_enter() != 0) return;
    pthread_mutex_lock(&g_sandbox_message_lock);
    set_message_locked(message);
    /* Publish the explanatory payload before making the completed state visible. */
    atomic_store_explicit(&g_sandbox_success, 0, memory_order_relaxed);
    atomic_store_explicit(&g_sandbox_complete, 1, memory_order_release);
    pthread_mutex_unlock(&g_sandbox_message_lock);
    tsd_runtime_safety_write_leave();
}

int tsd_runtime_flags_allow_transitions(void) {
    int complete = atomic_load_explicit(&g_sandbox_complete, memory_order_acquire);
    int success = atomic_load_explicit(&g_sandbox_success, memory_order_relaxed);
    if (!complete || !success) {
        return 0;
    }
    /*
     * The sandbox establishes executable-memory safety. Once telemetry fusion
     * is active, package-temperature availability is a second safety gate:
     * callers may still select/fall back to SSE4.1, but cannot authorize a
     * wider target while the thermal signal is absent.
     */
    return tsd_telemetry_temperature_upgrade_allowed();
}

int tsd_runtime_flags_sandbox_complete(void) {
    return atomic_load_explicit(&g_sandbox_complete, memory_order_acquire);
}

const char* tsd_runtime_flags_status_message(void) {
    pthread_mutex_lock(&g_sandbox_message_lock);
    (void)snprintf(g_sandbox_message_snapshot,
                   sizeof(g_sandbox_message_snapshot),
                   "%s",
                   g_sandbox_message);
    pthread_mutex_unlock(&g_sandbox_message_lock);
    return g_sandbox_message_snapshot;
}

void tsd_runtime_flags_set_sandbox_only(int enabled) {
    atomic_store_explicit(&g_sandbox_only, enabled ? 1 : 0, memory_order_release);
}

int tsd_runtime_flags_sandbox_only(void) {
    return atomic_load_explicit(&g_sandbox_only, memory_order_acquire);
}
