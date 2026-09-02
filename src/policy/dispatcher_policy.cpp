#include <thermal/simd/policy/dispatcher_policy.h>

#include <memory>

#include <observability/telemetry_state.h>

#include "mpc_controller.h"

namespace {

struct DispatcherPolicyState {
    tsd_policy_config config;
    bool fallback_active;
    bool last_temp_available;
    bool have_sample;
    std::unique_ptr<tsd::policy::MPCController> controller;

    DispatcherPolicyState()
        : config{}, fallback_active(false), last_temp_available(false), have_sample(false), controller(nullptr) {}
};

void publish_state(const DispatcherPolicyState &state,
                   simd_width_t current,
                   simd_width_t recommended,
                   bool changed) {
    tsd_controller_telemetry_t telemetry{};
    telemetry.fallback_active = state.fallback_active ? 1 : 0;
    telemetry.current_width = current;
    telemetry.recommended_width = recommended;
    telemetry.issued_change = changed ? 1 : 0;
    tsd_observability_update_controller(&telemetry);
}

}  // namespace

extern "C" {

tsd_dispatcher_policy_state* tsd_dispatcher_policy_create(const tsd_policy_config *config) {
    DispatcherPolicyState *state = nullptr;
    try {
        state = new DispatcherPolicyState();
        if (config) {
            state->config = *config;
        } else {
            tsd_policy_config defaults;
            tsd_policy_config_set_defaults(&defaults);
            state->config = defaults;
        }
        state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        state->fallback_active = false;
        publish_state(*state, SIMD_SSE41, SIMD_SSE41, false);
    } catch (...) {
        delete state;
        return nullptr;
    }
    return reinterpret_cast<tsd_dispatcher_policy_state*>(state);
}

void tsd_dispatcher_policy_destroy(tsd_dispatcher_policy_state *opaque) {
    if (!opaque) {
        return;
    }
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    delete state;
}

void tsd_dispatcher_policy_reset(tsd_dispatcher_policy_state *opaque, const tsd_policy_config *config) {
    if (!opaque) {
        return;
    }
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    if (config) {
        state->config = *config;
    }
    if (!state->controller) {
        try {
            state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        } catch (...) {
            state->fallback_active = true;
            publish_state(*state, SIMD_SSE41, SIMD_SSE41, false);
            return;
        }
    }
    state->controller->reset(state->config);
    state->fallback_active = false;
    state->last_temp_available = false;
    state->have_sample = false;
    publish_state(*state, SIMD_SSE41, SIMD_SSE41, false);
}

void tsd_dispatcher_policy_record(tsd_dispatcher_policy_state *opaque,
                                  const tsd_thermal_eval_t *sample,
                                  simd_width_t width) {
    if (!opaque || !sample) {
        return;
    }
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    state->last_temp_available = sample->temp_available != 0;
    state->have_sample = true;
    if (!state->controller) {
        try {
            state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        } catch (...) {
            state->fallback_active = true;
            publish_state(*state, width, width, false);
            return;
        }
    }
    state->controller->pushSample(*sample, width);
}

int tsd_dispatcher_policy_recommend(tsd_dispatcher_policy_state *opaque,
                                    simd_width_t current_width,
                                    simd_width_t max_width,
                                    simd_width_t *out_width,
                                    int *fallback_active) {
    if (fallback_active) {
        *fallback_active = 0;
    }
    if (out_width) {
        *out_width = current_width;
    }
    if (!opaque) {
        return 0;
    }
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    if (state->fallback_active) {
        if (fallback_active) {
            *fallback_active = 1;
        }
        publish_state(*state, current_width, current_width, false);
        return 0;
    }
    if (!state->controller) {
        try {
            state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        } catch (...) {
            state->fallback_active = true;
            if (fallback_active) {
                *fallback_active = 1;
            }
            publish_state(*state, current_width, current_width, false);
            return 0;
        }
    }
    simd_width_t target = current_width;
    bool changed = state->controller->recommend(current_width, max_width, target);
    if (!changed) {
        return 0;
    }

    /*
     * Missing package temperature must never authorize a wider SIMD width.
     * Downgrades remain available so degraded telemetry can still fail closed.
     */
    if (target > current_width && (!state->have_sample || !state->last_temp_available)) {
        publish_state(*state, current_width, current_width, false);
        return 0;
    }

    if (out_width) {
        *out_width = target;
    }
    publish_state(*state, current_width, target, true);
    return 1;
}

void tsd_dispatcher_policy_force_fallback(tsd_dispatcher_policy_state *opaque) {
    if (!opaque) {
        return;
    }
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    state->fallback_active = true;
    publish_state(*state, SIMD_SSE41, SIMD_SSE41, false);
}

}  // extern "C"
