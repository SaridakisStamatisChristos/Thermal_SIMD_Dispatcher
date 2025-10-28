#include <config/policy_config.h>

#ifndef TSD_DEFAULT_SLO_RATIO_MILLI
#define TSD_DEFAULT_SLO_RATIO_MILLI 1500U
#endif

#ifndef TSD_DEFAULT_SLO_TEMP_MILLIC
#define TSD_DEFAULT_SLO_TEMP_MILLIC 85000
#endif

#ifndef TSD_DEFAULT_TRANSITION_PENALTY_UP
#define TSD_DEFAULT_TRANSITION_PENALTY_UP 750U
#endif

#ifndef TSD_DEFAULT_TRANSITION_PENALTY_DOWN
#define TSD_DEFAULT_TRANSITION_PENALTY_DOWN 1000U
#endif

#ifndef TSD_DEFAULT_FORECAST_HORIZON
#define TSD_DEFAULT_FORECAST_HORIZON 5U
#endif

void tsd_policy_config_set_defaults(tsd_policy_config *cfg) {
    if (!cfg) {
        return;
    }
    cfg->slo_ratio_milli = TSD_DEFAULT_SLO_RATIO_MILLI;
    cfg->slo_temp_millic = TSD_DEFAULT_SLO_TEMP_MILLIC;
    cfg->transition_penalty_up_milli = TSD_DEFAULT_TRANSITION_PENALTY_UP;
    cfg->transition_penalty_down_milli = TSD_DEFAULT_TRANSITION_PENALTY_DOWN;
    cfg->forecast_horizon = TSD_DEFAULT_FORECAST_HORIZON;
}

void tsd_policy_config_apply_bounds(tsd_policy_config *cfg) {
    if (!cfg) {
        return;
    }
    if (cfg->forecast_horizon == 0) {
        cfg->forecast_horizon = 1;
    }
}
