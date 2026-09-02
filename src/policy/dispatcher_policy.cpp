#include <thermal/simd/policy/dispatcher_policy.h>

#include <memory>

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
    std::unique_ptr<tsd::policy::MPCController> controller;

    DispatcherPolicyState()
        : config{}, fallback_active(false), last_temp_available(false), last_temp_millic(0),
          have_sample(false), controller(nullptr) {}
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

bool runtime_temperature_limits_valid() {
    return g_tsd_config.predictive_temp_ceiling_c >= 20 &&
           g_tsd_config.predictive_temp_ceiling_c <= 125 &&
           g_tsd_config.predictive_safety_margin_c >= 0 &&
           g_tsd_config.predictive_emergency_margin_c >= 0;
}

int64_t upgrade_temperature_limit_millic() {
    if (!runtime_temperature_limits_valid()) {
        return INT64_MAX;
    }
    return static_cast<int64_t>(g_tsd_config.predictive_temp_ceiling_c -
                                g_tsd_config.predictive_safety_margin_c) * 1000;
}

int64_t emergency_temperature_limit_millic() {
    if (!runtime_temperature_limits_valid()) {
        return INT64_MAX;
    }
    return static_cast<int64_t>(g_tsd_config.predictive_temp_ceiling_c +
                                g_tsd_config.predictive_emergency_margin_c) * 1000;
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
    state->last_temp_millic = 0;
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
    state->last_temp_millic = sample->package_temp_millic;
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
    publish_state(*state, width, width, false);
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

    if (state->have_sample && state->last_temp_available && current_width > SIMD_SSE41 &&
        static_cast<int64_t>(state->last_temp_millic) >= emergency_temperature_limit_millic()) {
        if (out_width) {
            *out_width = SIMD_SSE41;
        }
        publish_state(*state, current_width, SIMD_SSE41, true);
        return 1;
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
        publish_state(*state, current_width, current_width, false);
        return 0;
    }

    /*
     * Missing package temperature or a temperature inside the configured
     * safety guard band must never authorize a wider SIMD width. Downgrades
     * remain available so degraded telemetry can still fail closed.
     */
    if (target > current_width &&
        (!state->have_sample || !state->last_temp_available ||
         static_cast<int64_t>(state->last_temp_millic) > upgrade_temperature_limit_millic())) {
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

void tsd_dispatcher_policy_heartbeat(tsd_dispatcher_policy_state *opaque,
                                     simd_width_t current_width) {
    if (!opaque) {
        return;
    }
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    publish_state(*state, current_width, current_width, false);
}

int tsd_dispatcher_policy_reload(tsd_dispatcher_policy_state *opaque) {
    if (!opaque) {
        return -1;
    }
    auto *state = reinterpret_cast<DispatcherPolicyState*>(opaque);
    if (!state->controller) {
        try {
            state->controller = std::make_unique<tsd::policy::MPCController>(state->config);
        } catch (...) {
            state->fallback_active = true;
            return -1;
        }
    }
    return state->controller->reloadCoefficients() ? 0 : -1;
}

}  // extern "C"
