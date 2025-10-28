#ifndef TSD_POLICY_CONFIG_H
#define TSD_POLICY_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t slo_ratio_milli;
    int32_t slo_temp_millic;
    uint32_t transition_penalty_up_milli;
    uint32_t transition_penalty_down_milli;
    uint32_t forecast_horizon;
} tsd_policy_config;

void tsd_policy_config_set_defaults(tsd_policy_config *cfg);
void tsd_policy_config_apply_bounds(tsd_policy_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* TSD_POLICY_CONFIG_H */
