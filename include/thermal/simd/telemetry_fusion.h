#ifndef TSD_TELEMETRY_FUSION_H
#define TSD_TELEMETRY_FUSION_H

#include <thermal/simd/telemetry_helper.h>

#ifdef __cplusplus
extern "C" {
#endif

int tsd_telemetry_fusion_start(void);
void tsd_telemetry_fusion_stop(void);
int tsd_telemetry_fusion_sample(tsd_telemetry_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TSD_TELEMETRY_FUSION_H */

