#ifndef TSD_DISPATCHER_POLICY_H
#define TSD_DISPATCHER_POLICY_H

#include <stddef.h>

#include <config/policy_config.h>
#include <thermal/simd/simd_width.h>
#include <thermal/simd/thermal_eval_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsd_dispatcher_policy_state tsd_dispatcher_policy_state;

tsd_dispatcher_policy_state* tsd_dispatcher_policy_create(const tsd_policy_config *config);
void tsd_dispatcher_policy_destroy(tsd_dispatcher_policy_state *state);
void tsd_dispatcher_policy_reset(tsd_dispatcher_policy_state *state, const tsd_policy_config *config);
void tsd_dispatcher_policy_record(tsd_dispatcher_policy_state *state,
                                  const tsd_thermal_eval_t *sample,
                                  simd_width_t width);
int tsd_dispatcher_policy_recommend(tsd_dispatcher_policy_state *state,
                                    simd_width_t current_width,
                                    simd_width_t max_width,
                                    simd_width_t *out_width,
                                    int *fallback_active);
void tsd_dispatcher_policy_force_fallback(tsd_dispatcher_policy_state *state);

#ifdef __cplusplus
}
#endif

#endif /* TSD_DISPATCHER_POLICY_H */
