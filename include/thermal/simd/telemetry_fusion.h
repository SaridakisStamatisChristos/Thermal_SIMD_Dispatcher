#ifndef TSD_TELEMETRY_FUSION_H
#define TSD_TELEMETRY_FUSION_H

#include <thermal/simd/telemetry_helper.h>

#ifdef __cplusplus
extern "C" {
#endif

int tsd_telemetry_fusion_start(void);
void tsd_telemetry_fusion_stop(void);

/*
 * Publish a normalized direct sample into the fusion bus. This gives platform
 * adapters a stable C boundary for contributing temperature/frequency data
 * without depending on the C++ collector implementation.
 */
int tsd_telemetry_fusion_publish_sample(const tsd_telemetry_sample_t *sample);

/*
 * Returns 0 only when at least one usable temperature/frequency signal is
 * available. Empty/degraded snapshots return -1 so callers can fall back to
 * their authoritative direct telemetry source.
 */
int tsd_telemetry_fusion_sample(tsd_telemetry_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TSD_TELEMETRY_FUSION_H */
