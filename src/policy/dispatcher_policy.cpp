#include <thermal/simd/policy/dispatcher_policy.h>

#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#ifdef TSD_ENABLE_TESTS
#include <atomic>
#include <stdexcept>
#endif

#include <observability/telemetry_state.h>
#include <thermal/simd/thermal_config.h>

#include "mpc_controller.h"

namespace {

struct DispatcherPolicyState {
    tsd_policy_config config;
    bool fallback_active;
    bool last_temp_available;
    int32_t last_temp_millic;
    bool have_sample;
    int temp_ceiling_c;
    int safety_margin_c;
    int emergency_margin_c;
    std::unique_ptr<tsd::policy::MPCController> controller;

    DispatcherPolicyState()
        : config{}, fallback_active(false), last_temp_available(false), last_temp_millic(0),
          have_sample(false), temp_ceiling_c(0), safety_margin_c(0), emergency_margin_c(0),
          controller(nullptr) {}
};

void publish_state(const DispatcherPolicyState &state,
                   simd_width_t current,
                   simd_width_t recommended,
                   bool changed) noexcept {
    tsd_controller_telemetry_t telemetry{};
    telemetry.fallback_active = state.fallback_active ? 1 : 0;
    telemetry.current_width = current;
    telemetry.recommended_width = recommended;
    telemetry.issued_change = changed ? 1 : 0;
    tsd_observability_update_controller(&telemetry);
}

void enter_fail_closed(DispatcherPolicyState *state,
                       simd_width_t current,
                       int error_code) noexcept {
    errno = error_code;
    if (!state) return;
    state->fallback_active = true;
    const simd_width_t target = current > SIMD_SSE41 ? SIMD_SSE41 : current;
    publish_state(*state, current, target, target != current);
}

void snapshot_runtime_limits(DispatcherPolicyState &state) noexcept {
    const tsd_runtime_config *runtime = tsd_runtime_config_active_snapshot();
    state.temp_ceiling_c = runtime->predictive_temp_ceiling_c;
    state.safety_margin_c = runtime->predictive_safety_margin_c;
    state.emergency_margin_c = runtime->predictive_emergency_margin_c;
}

bool runtime_temperature_limits_valid(const DispatcherPolicyState &state) {
    return state.temp_ceiling_c >= 20 && state.temp_ceiling_c <= 125 &&
           state.safety_margin_c >= 0 && state.safety_margin_c <= 60 &&
           state.emergency_margin_c >= 0 && state.emergency_margin_c <= 60;
}

int64_t upgrade_temperature_limit_millic(const DispatcherPolicyState &state) {
    if (!runtime_temperature_limits_valid(state)) return INT64_MIN;
    return static_cast<int64_t>(state.temp_ceiling_c - state.safety_margin_c) * 1000;
}

int64_t emergency_temperature_limit_millic(const DispatcherPolicyState &state) {
    if (!runtime_temperature_limits_valid(state)) return INT64_MIN;
    return (static_cast<int64_t>(state.temp_ceiling_c) +
            static_cast<int64_t>(state.emergency_margin_c)) * 1000;
}

#ifdef TSD_ENABLE_TESTS
std::atomic<int> g_test_throw_stage{0};

void maybe_throw_for_test(int stage) {
    int expected = stage;
    if (g_test_throw_stage.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
        throw std::runtime_error("forced policy exception");
    }
}
#else
void maybe_throw_for_test(int) noexcept {}
#endif

}  // namespace

extern "C" {

tsd_dispatcher_policy_state* tsd_dispatcher_policy_create(const tsd_policy_config *config) {
    DispatcherPolicyState *state = nullptr;
    try {
        maybe_throw_for_test(1);
        state = new DispatcherPolicyState();
        if (config) {
            state->config = *config;
        } else {
            tsd_policy_config defaults;
            tsd_policy_config_set_defaults(&defaults);
            state->config = defaults;
        }
        snapshot_runtime_limits(*state);
        state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        state->fallback_active = false;
        publish_state(*state, SIMD_SSE41, SIMD_SSE41, false);
    } catch (const std::bad_alloc &) {
        delete state;
        errno = ENOMEM;
        return nullptr;
    } catch (const std::exception &) {
        delete state;
        errno = EIO;
        return nullptr;
    } catch (...) {
        delete state;
        errno = EIO;
        return nullptr;
    }
    return reinterpret_cast<tsd_dispatcher_policy_state*>(state);
}

void tsd_dispatcher_policy_destroy(tsd_dispatcher_policy_state *opaque) {
    if (!opaque) return;
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    try {
        delete state;
    } catch (...) {
        /* Public C ABI functions must never propagate C++ exceptions. */
    }
}

void tsd_dispatcher_policy_reset(tsd_dispatcher_policy_state *opaque, const tsd_policy_config *config) {
    if (!opaque) return;
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    try {
        maybe_throw_for_test(2);
        if (config) state->config = *config;
        snapshot_runtime_limits(*state);
        if (!state->controller) {
            state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        }
        state->controller->reset(state->config);
        state->fallback_active = false;
        state->last_temp_available = false;
        state->last_temp_millic = 0;
        state->have_sample = false;
        publish_state(*state, SIMD_SSE41, SIMD_SSE41, false);
    } catch (const std::bad_alloc &) {
        enter_fail_closed(state, SIMD_SSE41, ENOMEM);
    } catch (const std::exception &) {
        enter_fail_closed(state, SIMD_SSE41, EIO);
    } catch (...) {
        enter_fail_closed(state, SIMD_SSE41, EIO);
    }
}

void tsd_dispatcher_policy_record(tsd_dispatcher_policy_state *opaque,
                                  const tsd_thermal_eval_t *sample,
                                  simd_width_t width) {
    if (!opaque || !sample) return;
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    try {
        maybe_throw_for_test(3);
        state->last_temp_available = sample->temp_available != 0;
        state->last_temp_millic = sample->package_temp_millic;
        state->have_sample = true;
        if (!state->controller) {
            state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        }
        state->controller->pushSample(*sample, width);
        publish_state(*state, width, width, false);
    } catch (const std::bad_alloc &) {
        enter_fail_closed(state, width, ENOMEM);
    } catch (const std::exception &) {
        enter_fail_closed(state, width, EIO);
    } catch (...) {
        enter_fail_closed(state, width, EIO);
    }
}

int tsd_dispatcher_policy_recommend(tsd_dispatcher_policy_state *opaque,
                                    simd_width_t current_width,
                                    simd_width_t max_width,
                                    simd_width_t *out_width,
                                    int *fallback_active) {
    if (fallback_active) *fallback_active = 0;
    if (out_width) *out_width = current_width;
    if (!opaque) return 0;

    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    try {
        maybe_throw_for_test(4);
        if (state->fallback_active) {
            if (fallback_active) *fallback_active = 1;
            const simd_width_t target = current_width > SIMD_SSE41 ? SIMD_SSE41 : current_width;
            if (out_width) *out_width = target;
            publish_state(*state, current_width, target, target != current_width);
            return target != current_width ? 1 : 0;
        }

        if (state->have_sample && state->last_temp_available && current_width > SIMD_SSE41 &&
            static_cast<int64_t>(state->last_temp_millic) >= emergency_temperature_limit_millic(*state)) {
            if (out_width) *out_width = SIMD_SSE41;
            publish_state(*state, current_width, SIMD_SSE41, true);
            return 1;
        }

        if (!state->controller) {
            state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        }
        simd_width_t target = current_width;
        bool changed = state->controller->recommend(current_width, max_width, target);
        if (!changed) {
            publish_state(*state, current_width, current_width, false);
            return 0;
        }

        /*
         * Missing package temperature, invalid safety configuration, or a
         * temperature inside the configured safety guard band must never
         * authorize a wider SIMD width. Downgrades remain available so
         * degraded telemetry can still fail closed.
         */
        if (target > current_width &&
            (!state->have_sample || !state->last_temp_available ||
             static_cast<int64_t>(state->last_temp_millic) > upgrade_temperature_limit_millic(*state))) {
            publish_state(*state, current_width, current_width, false);
            return 0;
        }

        if (out_width) *out_width = target;
        publish_state(*state, current_width, target, true);
        return 1;
    } catch (const std::bad_alloc &) {
        enter_fail_closed(state, current_width, ENOMEM);
    } catch (const std::exception &) {
        enter_fail_closed(state, current_width, EIO);
    } catch (...) {
        enter_fail_closed(state, current_width, EIO);
    }

    if (fallback_active) *fallback_active = 1;
    const simd_width_t target = current_width > SIMD_SSE41 ? SIMD_SSE41 : current_width;
    if (out_width) *out_width = target;
    return target != current_width ? 1 : 0;
}

void tsd_dispatcher_policy_force_fallback(tsd_dispatcher_policy_state *opaque) {
    if (!opaque) return;
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    try {
        state->fallback_active = true;
        publish_state(*state, SIMD_SSE41, SIMD_SSE41, false);
    } catch (...) {
        enter_fail_closed(state, SIMD_SSE41, EIO);
    }
}

void tsd_dispatcher_policy_heartbeat(tsd_dispatcher_policy_state *opaque,
                                     simd_width_t current_width) {
    if (!opaque) return;
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    try {
        publish_state(*state, current_width, current_width, false);
    } catch (...) {
        enter_fail_closed(state, current_width, EIO);
    }
}

int tsd_dispatcher_policy_reload(tsd_dispatcher_policy_state *opaque) {
    if (!opaque) {
        errno = EINVAL;
        return -1;
    }
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    try {
        maybe_throw_for_test(5);
        if (!state->controller) {
            state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        }
        if (!state->controller->reloadCoefficients()) {
            errno = EIO;
            return -1;
        }
        return 0;
    } catch (const std::bad_alloc &) {
        enter_fail_closed(state, SIMD_SSE41, ENOMEM);
        return -1;
    } catch (const std::exception &) {
        enter_fail_closed(state, SIMD_SSE41, EIO);
        return -1;
    } catch (...) {
        enter_fail_closed(state, SIMD_SSE41, EIO);
        return -1;
    }
}

#ifdef TSD_ENABLE_TESTS
void tsd_dispatcher_policy_test_force_exception(int stage) {
    g_test_throw_stage.store(stage, std::memory_order_release);
}
#endif

}  // extern "C"
