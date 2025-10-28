#ifndef TSD_STRESS_COMMON_H
#define TSD_STRESS_COMMON_H

#include <pthread.h>

#include <thermal/simd/simd_width.h>

#include "thermal_simd_test.h"

#ifdef __cplusplus
extern "C" {
#endif

int tsd_stress_prepare_runtime(void);
void tsd_stress_teardown_runtime(void);
int tsd_stress_patch(simd_width_t width);
int tsd_stress_inject_failure(tsd_patch_fail_stage_t stage, simd_width_t width);

extern pthread_mutex_t tsd_stress_patch_mutex;

#ifdef __cplusplus
}
#endif

#endif /* TSD_STRESS_COMMON_H */
